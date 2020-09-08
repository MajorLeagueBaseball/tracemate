#include "tm_url_squasher.h"
#include "tm_metric.h"

#include <assert.h>
#include <ck_hs.h>
#include <ck_stack.h>
#include <mtev_rand.h>
#include <mtev_log.h>
#include <stdio.h>
#include <pcre.h>
#include <pthread.h>

typedef struct tm_ps_tree_node {
  bool children_squashed;
  struct tm_ps_tree_node *parent;
  union {
    ck_hs_t children;
    struct tm_ps_tree_node *squashed_child;
  } u;
  size_t segment_name_len;
  char segment_name[];
} node_t;

struct tm_path_squasher {
  pthread_mutex_t lock;
  node_t *root_node;
  int cardinality_factor;
  int added_path_count;
};

static unsigned long
hs_hash(const void *object, unsigned long seed)
{
  const node_t *c = object;
  unsigned long h;

  h = (unsigned long)mtev_hash__hash(c->segment_name, c->segment_name_len, seed);
  return h;
}

static bool
hs_compare(const void *previous, const void *compare)
{
  const node_t *prev_key = previous;
  const node_t *cur_key = compare;

  if (prev_key->segment_name_len == cur_key->segment_name_len) {
    return memcmp(prev_key->segment_name, cur_key->segment_name,
                  prev_key->segment_name_len) == 0;
  }

  /* We know they're not equal if they have different lengths */
  return false;
}

static void *
hs_malloc(size_t r)
{
  return malloc(r);
}

static void
hs_free(void *p, size_t b, bool r)
{
  (void)b;
  (void)r;
  free(p);
}

static struct ck_malloc my_allocator =
{
 .malloc = hs_malloc,
 .free = hs_free
};


static node_t*
create_node(const char *segment_name)
{
  node_t *n = calloc(1, sizeof(node_t) + strlen(segment_name) + 1);
  n->segment_name_len = strlen(segment_name);
  memcpy(n->segment_name, segment_name, n->segment_name_len + 1);
  ck_hs_init(&n->u.children, CK_HS_MODE_OBJECT | CK_HS_MODE_SPMC, hs_hash, hs_compare,
             &my_allocator, 25, mtev_rand());
  return n;
}

static void
destroy_node(node_t *node)
{
  if (!node->children_squashed) {
    ck_hs_destroy(&node->u.children);
  } 
  free(node);
}

static void
recursively_destroy(node_t *n)
{
  if (n == NULL) return;

  if (n->children_squashed) {
    recursively_destroy(n->u.squashed_child);
  }
  else {
    ck_hs_iterator_t it = CK_HS_ITERATOR_INITIALIZER;
    node_t *child_node;
    while(ck_hs_next(&n->u.children, &it, (void **)&child_node)) {
      recursively_destroy(child_node);
    }
  }
  destroy_node(n);
}

static node_t *
maybe_add_child(node_t *parent, const char *segment_name)
{
  assert(parent != NULL);

  if (parent->children_squashed) return parent->u.squashed_child;

  node_t *temp = (node_t *)alloca(sizeof(node_t) + strlen(segment_name) + 1);
  temp->segment_name_len = strlen(segment_name);
  memcpy(temp->segment_name, segment_name, temp->segment_name_len);

  /* check parent for existence of child */
  long hashv = CK_HS_HASH(&parent->u.children, hs_hash, temp);
  node_t *ex = ck_hs_get(&parent->u.children, hashv, temp);
  if (ex) return ex;

  ex = create_node(segment_name);
  ex->parent = parent;
  ck_hs_put(&parent->u.children, hashv, ex);
  return ex;
}

tm_path_squasher_t *tm_path_squasher_alloc(const int cardinality_factor)
{
  node_t *root_node = create_node("<ROOT>");
  tm_path_squasher_t *ps = calloc(1, sizeof(tm_path_squasher_t));
  ps->cardinality_factor = cardinality_factor;
  ps->root_node = root_node;
  pthread_mutex_init(&ps->lock, NULL);
  return ps;
}

void tm_path_squasher_destroy(tm_path_squasher_t *ps)
{
  recursively_destroy(ps->root_node);
  free(ps);
}

static void
recurse_add_children(node_t *parent, node_t *child)
{
  node_t *foo = maybe_add_child(parent, child->segment_name);

  if (child->children_squashed) {
    recurse_add_children(foo, child->u.squashed_child);
  }
  else {
    ck_hs_iterator_t child_it = CK_HS_ITERATOR_INITIALIZER;
    node_t *grandchild;
    while(ck_hs_next(&child->u.children, &child_it, (void **)&grandchild)) {
      recurse_add_children(foo, grandchild);
    }
  }
  destroy_node(child);
}

static node_t *
squash_children(tm_path_squasher_t *ps, node_t *parent)
{
  /*
   * create a new node to represent the squashed children.
   * then reparent all the children's children to the new squashed node.
   */
  node_t *new_child = create_node("{...}");
  ck_hs_iterator_t it = CK_HS_ITERATOR_INITIALIZER;
  node_t *old_child;
  while(ck_hs_next(&parent->u.children, &it, (void **)&old_child)) {
    if (old_child->children_squashed) {
      recurse_add_children(new_child, old_child->u.squashed_child);
    } 
    else {
      ck_hs_iterator_t child_it = CK_HS_ITERATOR_INITIALIZER;
      node_t *grandchild;
      while(ck_hs_next(&old_child->u.children, &child_it, (void **)&grandchild)) {
        recurse_add_children(new_child, grandchild);
      }
    }
    destroy_node(old_child);
  }
  if (!parent->children_squashed) {
    ck_hs_destroy(&parent->u.children);
  }
  new_child->parent = parent;
  parent->children_squashed = true;
  parent->u.squashed_child = new_child;
  return new_child;
}

int
tm_path_squasher_get_seen_count(tm_path_squasher_t *ps)
{
  return ps->added_path_count;
}

void tm_path_squasher_add_path(tm_path_squasher_t *ps, const char *url_path)
{
  /* incoming url_path has to be split along '/' chars and each
   * segment inserted into the tree.  if we encounter a child location
   * during inserts that eclipses the cardinality setting, we have to 
   * squash that level.
   * 
   * The squash semantics will replace the children with another string
   * doing it's best to match the content of the children with a known
   * set of regular expressions so that lists of integers will be replaced
   * by: "{integer}" and lists of guids will be replaced by: "{guid}", etc..
   * All unmatching lists will be replaced by: "{...}"
   * 
   * When tokenizing the incoming path, blank levels will be eliminated.  
   * For example: `/a/b//c//d` -> `/a/b/c/d`
   */
  pthread_mutex_lock(&ps->lock);
  ps->added_path_count++;
  char *temp = strdup(url_path);
  if (temp[strlen(temp)-1] == '\n') temp[strlen(temp) - 1] = '\0';

  char *segment, *brk;
  node_t *parent = ps->root_node;
  int depth = 0;
  for (segment = strtok_r(temp, "/", &brk);
       segment;
       segment = strtok_r(NULL, "/", &brk)) {
    if (*segment == '\0') continue;
    node_t *child = maybe_add_child(parent, segment);
    if (!parent->children_squashed) {
      if (ck_hs_count(&parent->u.children) > (ps->cardinality_factor / (3 << depth)) && parent != ps->root_node) {
        child = squash_children(ps, parent);
      }
    }
    depth++;
    if (depth == 5) {
      break;
    }
    parent = child;
  }
  free(temp);
  pthread_mutex_unlock(&ps->lock);
}

typedef struct stacker {
  ck_stack_entry_t entry;
  node_t *node;
} node_stack_entry_t;


static void
recurse_regex(mtev_hash_table *t, tm_path_squasher_t *ps, node_t *node, ck_stack_t *stack)
{
  if (node == NULL) return;

  bool EOL = false;
  if (node->children_squashed) {
    if (node->u.squashed_child == NULL) {
      /* this is the end of the line. */
      EOL = true;
    } else {
      node_stack_entry_t *n = malloc(sizeof(node_stack_entry_t));
      n->node = node->u.squashed_child;
      ck_stack_push_upmc(stack, &n->entry);
    }
  } else {
    if (ck_hs_count(&node->u.children) == 0) {
      /* this is the end of the line */
      EOL = true;
    } else {
      ck_hs_iterator_t it = CK_HS_ITERATOR_INITIALIZER;
      node_t *child;
      while(ck_hs_next(&node->u.children, &it, (void **)&child)) {
        node_stack_entry_t *n = malloc(sizeof(node_stack_entry_t));
        n->node = child;
        ck_stack_push_upmc(stack, &n->entry);
      }
    }
  }
  if (EOL) {
    ck_stack_t stack = CK_STACK_INITIALIZER;
    node_t *p = node;
    bool produce_regex = false;
    while(p) {
      if (p->children_squashed) produce_regex = true;
      struct stacker *s = calloc(1, sizeof(struct stacker));
      s->node = p;
      ck_stack_push_upmc(&stack, &s->entry);
      p = p->parent;
    }

    char regex[512] = {0};
    char replace[512] = {0};
    ck_stack_entry_t *e = NULL;
    const char *pcre_err;
    int erroff;

    if (produce_regex) {
      strcat(regex, "(");
      while ((e = ck_stack_pop_upmc(&stack)) != NULL) {
        struct stacker *s = (struct stacker *)e;
        if (s->node == ps->root_node) {
          free(s);
          continue;
        }
        if (s->node->parent != ps->root_node) {
          strcat(regex, "\\/");
          strcat(replace, "/");
        }
        if (strcmp(s->node->segment_name, "{...}") == 0) {
          strcat(regex, "[^\\/]+");
          strcat(replace, "{...}");
        } else {
          strcat(regex, s->node->segment_name);
          strcat(replace, s->node->segment_name);
        }
        free(s);
      }
      strcat(regex, ".*)");

      pcre *match = pcre_compile(regex, 0, &pcre_err, &erroff, NULL);
      if(!match) {
        mtevL(mtev_error, "pcre_compiled failed offset %d: %s on (%s)\n", erroff, pcre_err, regex);
        return;
      }

      pcre_matcher *pcrem = (pcre_matcher *)calloc(1, sizeof(pcre_matcher));
      pcrem->match = match;
      pcrem->extra = pcre_study(pcrem->match, 0, &pcre_err);
      pcrem->replace = strdup(replace);
      mtev_hash_store(t, strdup(regex), strlen(regex) + 1, pcrem);
    }
  }
}

mtev_hash_table*
tm_path_squasher_get_regexes(tm_path_squasher_t *ps)
{
  mtev_hash_table *t = calloc(1, sizeof(mtev_hash_table));
  mtev_hash_init_locks(t, 200, MTEV_HASH_LOCK_MODE_NONE);
  node_t *child;
  ck_hs_iterator_t it = CK_HS_ITERATOR_INITIALIZER;
  ck_stack_t stack = CK_STACK_INITIALIZER;

  pthread_mutex_lock(&ps->lock);
  while(ck_hs_next(&ps->root_node->u.children, &it, (void **)&child)) {
    node_stack_entry_t *n = malloc(sizeof(node_stack_entry_t));
    n->node = child;
    ck_stack_push_upmc(&stack, &n->entry);
  }

  ck_stack_entry_t *stack_entry = NULL;
  while ((stack_entry = ck_stack_pop_upmc(&stack)) != NULL) {
    node_stack_entry_t *node = (node_stack_entry_t *)stack_entry;
    recurse_regex(t, ps, node->node, &stack);
    free(node);
  }
  pthread_mutex_unlock(&ps->lock);

  return t;
}


static void
recurse_print(tm_path_squasher_t *ps, node_t *node)
{
  if (node == NULL) return;

  bool EOL = false;
  if (node->children_squashed) {
    recurse_print(ps, node->u.squashed_child);
    if (node->u.squashed_child == NULL) {
      /* this is the end of the line. */
      EOL = true;
    }
  } else {
    ck_hs_iterator_t it = CK_HS_ITERATOR_INITIALIZER;
    node_t *child;
    while(ck_hs_next(&node->u.children, &it, (void **)&child)) {
      recurse_print(ps, child);
    }
    if (ck_hs_count(&node->u.children) == 0) {
      /* this is the end of the line */
      EOL = true;
    }
  }
  if (EOL) {
    ck_stack_t stack = CK_STACK_INITIALIZER;
    node_t *p = node;
    while(p) {
      struct stacker *s = calloc(1, sizeof(struct stacker));
      s->node = p;
      ck_stack_push_upmc(&stack, &s->entry);
      p = p->parent;
    }

    ck_stack_entry_t *e = NULL;
    while ((e = ck_stack_pop_upmc(&stack)) != NULL) {
      struct stacker *s = (struct stacker *)e;
      printf("/");
      printf(s->node->segment_name);
      free(s);
    }
    printf("\n");
  }
}

void tm_path_squasher_print_tree(tm_path_squasher_t *ps)
{
  node_t *child;
  ck_hs_iterator_t it = CK_HS_ITERATOR_INITIALIZER;
  pthread_mutex_lock(&ps->lock);
  while(ck_hs_next(&ps->root_node->u.children, &it, (void **)&child)) {
    recurse_print(ps, child);
  }
  pthread_mutex_unlock(&ps->lock);

}
