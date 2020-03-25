/**
 * Copyright 2020 Major League Baseball
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tm_transaction_store.h"

#include "tm_log.h"
#include "tm_jaeger.h"
#include "tm_metric.h"
#include "tm_process.h"

#include <mtev_hash.h>

#include <lmdb.h>
#include <errno.h>
#include <mtev_mkdir.h>
#include <stdio.h>

struct tm_transaction_store_iter {
  tm_transaction_store_type type;
  MDB_cursor *cursor;
  MDB_val key;
  MDB_val data;
  mtev_boolean before_first;
  mtev_hash_iter hash_iter;
};

static MDB_env *env;
static MDB_dbi dbi;

static tm_transaction_store_type tm_type;
static pthread_mutex_t lock;
static int default_lookback_secs;

typedef struct stored {
  uint64_t first_seen_ms;
  bool trace;
  mtev_json_object *json;
  char data[];
} stored_t;

typedef struct lmdb_stored_v1 {
    size_t json_str_len;
    uint64_t first_seen_ms;
    bool trace;
    char data[];
} lmdb_stored_v1_t;

#define MAGIC_WORD_V2 0xd06f00d
#define MAGIC_WORD_V3 0xbadf00d
#define MAGIC_WORD_V4 0x0fa7a55
typedef struct lmdb_stored_root_v2 {
  int magic_word;
  size_t json_str_len;
  uint64_t first_seen_ms;
  uint64_t last_modified_ms;
  bool trace;
  char data[];
} __attribute__((packed)) lmdb_stored_root_v2_t;

typedef struct lmdb_stored_root_v3 {
  int magic_word;
  size_t json_str_len;
  uint64_t first_seen_ms;
  uint64_t last_modified_ms;
  bool trace;
  char data[];
} __attribute__((packed)) lmdb_stored_root_v3_t;

typedef struct lmdb_stored_root_v4 {
  int magic_word;
  size_t json_str_len;
  uint64_t first_seen_ms;
  uint64_t last_modified_ms;
  int ttl;
  bool trace;
  char data[];
} __attribute__((packed)) lmdb_stored_root_t;


typedef struct lmdb_stored_child_v2 {
  int magic_word;
  size_t json_str_len;
  char data[];
} __attribute__((packed)) lmdb_stored_child_v2_t;

typedef struct lmdb_stored_child_v3 {
  int magic_word;
  size_t json_str_len;
  uint64_t first_seen_ms;
  char data[];
} __attribute__((packed)) lmdb_stored_child_v3_t;

typedef struct lmdb_stored_child_v4 {
  int magic_word;
  size_t json_str_len;
  uint64_t first_seen_ms;
  int ttl;
  char data[];
} __attribute__((packed)) lmdb_stored_child_t;

static bool
_mkdir(const char *path)
{
  char to_make[PATH_MAX];
  size_t copy_len = strlen(path);
  memset(to_make, 0, PATH_MAX);
  memcpy(to_make, path, MIN(copy_len, PATH_MAX));
  strlcat(to_make, "/dummy", sizeof(to_make));
  if (mkdir_for_file(to_make, 0777)) {
    mtevL(mtev_error, "mkdir %s: %s\n", to_make, strerror(errno));
    return false;
  }
  return true;
}



void
tm_transaction_store_init(tm_transaction_store_type type, const char *path, 
                          size_t initial_size, int _default_lookback_secs)
{
  tm_type = type;
  default_lookback_secs = _default_lookback_secs;
  pthread_mutex_init(&lock, NULL);

  if (!_mkdir(path)) {
    mtevFatal(tm_error, "Could not create transaction store dir: %s\n", path);
  }

  int rc = mdb_env_create(&env);
  mtevAssert(rc == 0);

  /* let lots of threads read us */
  rc = mdb_env_set_maxreaders(env, 1024);
  mtevAssert(rc == 0);

  /* open more than 1 database */
  rc = mdb_env_set_maxdbs(env, 5);
  mtevAssert(rc == 0);

  rc = mdb_env_set_mapsize(env, initial_size  - (initial_size % 4096));
  mtevAssert(rc == 0);

  rc = mdb_env_open(env, path, MDB_NORDAHEAD | MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOMEMINIT, 0644);
  mtevAssert(rc == 0);

  MDB_txn *txn;
  rc = mdb_txn_begin(env, NULL, 0, &txn);
  mtevAssert(rc == 0);
  rc = mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
  mtevAssert(rc == 0);

  rc = mdb_txn_commit(txn);
  mtevAssert(rc == 0);
}

void
tm_transaction_store_close()
{
  mdb_dbi_close(env, dbi);
  mdb_env_close(env);
}

bool
tm_transaction_store_put(const char *id, size_t id_len, tm_transaction_store_entry_t *entry, int ttl)
{
  MDB_val key, data;
  MDB_txn *txn;
  MDB_cursor *cursor;
  int rc;

  if (ttl == 0) ttl = default_lookback_secs + 300;

  const char *json_string = mtev_json_object_to_json_string(entry->data);
  size_t json_string_len = strlen(json_string);
  size_t s_size = sizeof(lmdb_stored_root_t) + json_string_len;
  lmdb_stored_root_t *s = (lmdb_stored_root_t *)malloc(s_size);
  s->magic_word = MAGIC_WORD_V4;
  s->first_seen_ms = entry->first_seen_ms;
  s->last_modified_ms = mtev_now_ms();
  s->json_str_len = json_string_len;
  s->trace = entry->trace;
  s->ttl = ttl;
  memcpy(s->data, json_string, json_string_len);

  rc = mdb_txn_begin(env, NULL, 0, &txn);
  if (rc != 0) {
    return false;
  }

  rc = mdb_cursor_open(txn, dbi, &cursor);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return false;
  }

  key.mv_data = (void *)id;
  key.mv_size = id_len;

  data.mv_data = (void *)s;
  data.mv_size = s_size;

  rc = mdb_cursor_put(cursor, &key, &data, 0);
  if (rc != 0) {
    mtevL(tm_error, "Failure to put %s, error: %s\n", id, mdb_strerror(rc));
    mdb_txn_abort(txn);
    free(s);
    return false;
  }
  mdb_cursor_close(cursor);
  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    mtevL(tm_error, "Failure to commit %s, error: %s\n", id, mdb_strerror(rc));
    free(s);
    return false;
  }
  free(s);
  return true;
}

bool
tm_transaction_store_add_child(const char *trace_id, size_t id_len, mtev_json_object *child, int ttl)
{
  char root_key[96];
  size_t root_key_len = 0;
  MDB_val key, data;
  MDB_txn *txn;
  MDB_cursor *cursor;
  int rc;

  if (child == NULL) return false;

  if (ttl == 0) ttl = (default_lookback_secs + 300);

  rc = mdb_txn_begin(env, NULL, 0, &txn);
  if (rc != 0) {
    return false;
  }

  rc = mdb_cursor_open(txn, dbi, &cursor);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return false;
  }

  key.mv_data = (void *)trace_id;
  key.mv_size = id_len;
  rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);
  if (rc != 0 && rc != MDB_NOTFOUND) {
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return NULL;
  }

  if (rc == 0) {
    int *mw = (int *)data.mv_data;
    lmdb_stored_root_t *copy = NULL;
    if (*mw == MAGIC_WORD_V3) {
      lmdb_stored_root_v3_t *s = (lmdb_stored_root_v3_t *)data.mv_data;
      copy = (lmdb_stored_root_t *)malloc(sizeof(lmdb_stored_root_t) + s->json_str_len);
      copy->first_seen_ms = s->first_seen_ms;
      copy->json_str_len = s->json_str_len;
      copy->trace = s->trace;
      memcpy(copy->data, s->data, s->json_str_len);
      copy->ttl = ttl;
    } else {
      lmdb_stored_root_t *s = (lmdb_stored_root_t *)data.mv_data;
      copy = (lmdb_stored_root_t *)malloc(sizeof(lmdb_stored_root_t) + s->json_str_len);
      memcpy(copy, s, data.mv_size);
    }
    copy->magic_word = MAGIC_WORD_V4;
    copy->last_modified_ms = mtev_now_ms();
    data.mv_data = copy;
    data.mv_size = sizeof(lmdb_stored_root_t) + copy->json_str_len;
    rc = mdb_cursor_put(cursor, &key, &data, 0);
    if (rc != 0) {
      mtevL(tm_error, "Failure to put %s, error: %s\n", trace_id, mdb_strerror(rc));
      mdb_txn_abort(txn);
      free(copy);
      return false;
    }
    free(copy);
  }
  const char *json_string = mtev_json_object_to_json_string(child);
  size_t json_string_len = strlen(json_string);
  size_t s_size = sizeof(lmdb_stored_child_t) + json_string_len;
  lmdb_stored_child_t *sc = (lmdb_stored_child_t *)malloc(s_size);
  sc->magic_word = MAGIC_WORD_V4;
  sc->first_seen_ms = mtev_now_ms();
  sc->json_str_len = json_string_len;
  sc->ttl = ttl;
  memcpy(sc->data, json_string, json_string_len);

  mtev_json_object *processor = mtev_json_object_object_get(child, "processor");
  mtev_json_object *event = mtev_json_object_object_get(processor, "event");
  if (event) {
    const char *ev = mtev_json_object_get_string(event);
    if (strcmp(ev, "span") == 0) {
      mtev_json_object *span = mtev_json_object_object_get(child, "span");
      mtev_json_object *id = mtev_json_object_object_get(span, "hex_id");
      const char *hex_id = mtev_json_object_get_string(id);
      root_key_len = snprintf(root_key, sizeof(root_key), "%.*s-%s", (int)id_len, trace_id, hex_id);
    }
    else if (strcmp(ev, "error") == 0) {
      mtev_json_object *error = mtev_json_object_object_get(child, "error");
      mtev_json_object *id = mtev_json_object_object_get(error, "id");
      const char *hex_id = mtev_json_object_get_string(id);
      root_key_len = snprintf(root_key, sizeof(root_key), "%.*s-%s", (int)id_len, trace_id, hex_id);
    }
    else if (strcmp(ev, "transaction") == 0) {
      mtev_json_object *trans = mtev_json_object_object_get(child, "transaction");
      mtev_json_object *id = mtev_json_object_object_get(trans, "id");
      const char *hex_id = mtev_json_object_get_string(id);
      root_key_len = snprintf(root_key, sizeof(root_key), "%.*s-%s", (int)id_len, trace_id, hex_id);
    }
  }

  key.mv_data = (void *)root_key;
  key.mv_size = root_key_len;

  data.mv_data = (void *)sc;
  data.mv_size = s_size;

  rc = mdb_cursor_put(cursor, &key, &data, 0);
  if (rc != 0) {
    mtevL(tm_error, "Failure to put %s, error: %s\n", trace_id, mdb_strerror(rc));
    mdb_txn_abort(txn);
    free(sc);
    return false;
  }
  mdb_cursor_close(cursor);
  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    mtevL(tm_error, "Failure to commit %s, error: %s\n", trace_id, mdb_strerror(rc));
    free(sc);
    return false;
  }
  free(sc);
  return true;
}


tm_transaction_store_entry_t *
tm_transaction_store_get(const char *id, size_t id_len)
{
  MDB_val key, data;
  MDB_txn *txn;
  MDB_cursor *cursor;
  int rc;

  rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
  mtevAssert(rc == 0);

  rc = mdb_cursor_open(txn, dbi, &cursor);
  mtevAssert(rc == 0);

  key.mv_data = (void *)id;
  key.mv_size = id_len;
  rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);
  if (rc != 0) {
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return NULL;
  }

  tm_transaction_store_entry_t *rval = NULL;
  int *mw = (int *)data.mv_data;
  if (*mw == MAGIC_WORD_V3) {
    lmdb_stored_root_v3_t *s = (lmdb_stored_root_v3_t *)data.mv_data;
    /* we have to copy because the calling process treats this as a mutable object */
    rval = (tm_transaction_store_entry_t *)calloc(1, sizeof(tm_transaction_store_entry_t));

    mtev_json_tokener *tok = mtev_json_tokener_new();
    mtev_json_object *json = mtev_json_tokener_parse_ex(tok, (const char *)s->data, s->json_str_len);
    mtev_json_tokener_free(tok);
    rval->first_seen_ms = s->first_seen_ms;
    rval->last_modified_ms = s->last_modified_ms;
    rval->trace = s->trace;
    rval->data = json;
  } else {
    lmdb_stored_root_t *s = (lmdb_stored_root_t *)data.mv_data;
    /* we have to copy because the calling process treats this as a mutable object */
    rval = (tm_transaction_store_entry_t *)calloc(1, sizeof(tm_transaction_store_entry_t));

    mtev_json_tokener *tok = mtev_json_tokener_new();
    mtev_json_object *json = mtev_json_tokener_parse_ex(tok, (const char *)s->data, s->json_str_len);
    mtev_json_tokener_free(tok);
    rval->first_seen_ms = s->first_seen_ms;
    rval->last_modified_ms = s->last_modified_ms;
    rval->trace = s->trace;
    rval->data = json;
  }
  mdb_cursor_close(cursor);
  mdb_txn_abort(txn);

  return rval;
}

static size_t
_tm_transaction_store_get_children(MDB_txn *txn, const char *id, size_t id_len, tm_transaction_store_entry_t **children)
{
  MDB_val key, data;
  MDB_cursor *cursor;
  MDB_cursor_op op = MDB_SET_RANGE;
  int rc;

  rc = mdb_cursor_open(txn, dbi, &cursor);
  mtevAssert(rc == 0);

  size_t child_count = 0;
  size_t child_alloc_count = 10;
  *children = calloc(child_alloc_count, sizeof(tm_transaction_store_entry_t));

  key.mv_data = (void *)id;
  key.mv_size = id_len;
  while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
    op = MDB_NEXT;
    if (strncmp(id, (const char *)key.mv_data, id_len) != 0) {
      break;
    }

    if (key.mv_size == id_len) {
      continue; // skip the root key
    }

    int *mw = (int *)data.mv_data;
    mtev_json_object *json = NULL;
    if (*mw == MAGIC_WORD_V3) {
      lmdb_stored_child_v3_t *s = (lmdb_stored_child_v3_t *)data.mv_data;

      mtev_json_tokener *tok = mtev_json_tokener_new();
      json = mtev_json_tokener_parse_ex(tok, (const char *)s->data, s->json_str_len);
      mtev_json_tokener_free(tok);
    } else if (*mw == MAGIC_WORD_V4) {
      lmdb_stored_child_t *s = (lmdb_stored_child_t *)data.mv_data;

      mtev_json_tokener *tok = mtev_json_tokener_new();
      json = mtev_json_tokener_parse_ex(tok, (const char *)s->data, s->json_str_len);
      mtev_json_tokener_free(tok);
    }

    if (json != NULL) {
      (*children)[child_count].data = json;
      child_count++;
      if (child_count >= child_alloc_count) {
        child_alloc_count *= 2;
        *children = realloc(*children, child_alloc_count * sizeof(tm_transaction_store_entry_t));
      }
    }
  }
  mdb_cursor_close(cursor);

  return child_count;
}

size_t
tm_transaction_store_get_children(const char *id, size_t id_len, tm_transaction_store_entry_t **children)
{
  MDB_txn *txn;
  int rc;

  rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
  mtevAssert(rc == 0);

  size_t c = _tm_transaction_store_get_children(txn, id, id_len, children);
  mdb_txn_abort(txn);
  return c;
}

size_t
tm_transaction_store_delete_old_transactions()
{
#define COUNT 500
  uint64_t now = mtev_now_ms();
  int rc;
  size_t expired_count = 0;
  MDB_txn *txn;
  MDB_cursor *cursor;
  MDB_cursor_op op = MDB_FIRST;
  MDB_val key, data;

  /* 1 pass.. 
   * 
   * pass: find all the entries that should be deleted and delete them.
   * 
   * By keeping the max count small we can afford this transaction lock
   */
  rc = mdb_txn_begin(env, NULL, 0, &txn);
  if (rc != 0) {
    return 0;
  }

  rc = mdb_cursor_open(txn, dbi, &cursor);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return 0;
  }

  while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
    op = MDB_NEXT;

    int *mw = (int *)data.mv_data;
    if (*mw == MAGIC_WORD_V4 && key.mv_size > 32) {
      /* child of v4 type, this type has a ttl stored with it */
      lmdb_stored_child_t *child = (lmdb_stored_child_t *)data.mv_data;
      uint64_t time_ms = now - ((uint64_t)child->ttl * 1000UL);
      if (child->first_seen_ms <= time_ms) {
        mdb_cursor_del(cursor, 0);
        expired_count++;
        if (expired_count == COUNT) break;
      }
    } else if (key.mv_size == 32 && *mw == MAGIC_WORD_V4) {
      /* this is a parent record of v4 type, use it's ttl */
      lmdb_stored_root_t *s = (lmdb_stored_root_t *)data.mv_data;
      uint64_t time_ms = now - ((uint64_t)s->ttl * 1000UL);
      if (s->first_seen_ms <= time_ms) {
        mdb_cursor_del(cursor, 0);
        expired_count++;
        if (expired_count == COUNT) break;
      }
    } else {
      mdb_cursor_del(cursor, 0);
      expired_count++;
      if (expired_count == COUNT) break;
    }
  }
  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    mtevL(tm_error, "Error committing delete trans: %d\n", rc);
  }

  uint64_t done = mtev_now_ms();
  mtevL(mtev_error, "Clean took: %" PRIu64 " ms\n", (done - now));
  return expired_count;
}

size_t
tm_transaction_store_process_jaeger()
{
#define JAEGER_MAX_COUNT 4000
  int rc;
  size_t jaegered_count = 0;
  MDB_txn *txn;
  MDB_cursor *cursor;
  MDB_cursor_op op = MDB_FIRST;
  MDB_val key, data;
  uint64_t time_ms = mtev_now_ms() - (uint64_t)(default_lookback_secs * 1000UL);

  rc = mdb_txn_begin(env, NULL, 0, &txn);
  if (rc != 0) {
    return 0;
  }

  rc = mdb_cursor_open(txn, dbi, &cursor);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return 0;
  }

  char root_span_team[128];
  char span_team[128];
  while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
    op = MDB_NEXT;
    if (key.mv_size > 32) continue;

    int *mw = (int *)data.mv_data;
    mtev_json_object *json = NULL;
    lmdb_stored_root_t *s = NULL;
    if (*mw == MAGIC_WORD_V3) {
      lmdb_stored_root_v3_t *ss = (lmdb_stored_root_v3_t *)data.mv_data;
      if (ss->last_modified_ms <= time_ms && ss->trace == true) {
        s = (lmdb_stored_root_t *)calloc(1, sizeof(lmdb_stored_root_t) + ss->json_str_len);
        s->magic_word = MAGIC_WORD_V4;
        s->json_str_len = ss->json_str_len;
        s->first_seen_ms = ss->first_seen_ms;
        s->last_modified_ms = ss->last_modified_ms;
        s->ttl = default_lookback_secs + 300;
        s->trace = ss->trace;
        memcpy(s->data, ss->data, ss->json_str_len);

        mtev_json_tokener *tok = mtev_json_tokener_new();
        json = mtev_json_tokener_parse_ex(tok, (const char *)ss->data, ss->json_str_len);
        mtev_json_tokener_free(tok);

      }
    } else {
      lmdb_stored_root_t *ss = (lmdb_stored_root_t *)data.mv_data;
      if (ss->last_modified_ms <= time_ms && ss->trace == true) {
        s = (lmdb_stored_root_t *)calloc(1, sizeof(lmdb_stored_root_t) + ss->json_str_len);
        mtev_json_tokener *tok = mtev_json_tokener_new();
        json = mtev_json_tokener_parse_ex(tok, (const char *)ss->data, ss->json_str_len);
        mtev_json_tokener_free(tok);
        memcpy(s, ss, data.mv_size);
      }
    }
    if (json != NULL) {
      mtev_json_object *trans = mtev_json_object_object_get(json, "transaction");
      if (trans) {
        bool rejaegered_root = false;
        const char *service_name = tm_service_name(trans);
        if (tm_get_team(service_name, root_span_team)) {
          team_data_t *td = get_team_data(root_span_team);
          jaegerize_transaction(trans, td->jaeger_dest_url);
          jaegered_count++;

          tm_transaction_store_entry_t *children = NULL;
          size_t child_count = _tm_transaction_store_get_children(txn, key.mv_data, key.mv_size, &children);
          for (size_t i = 0; i < child_count; i++) {
            /*
             * get all the related teams in this transaction.. we are going to jaeger the spans to every
             * team (copy them) so that the separate jaeger instances have full visibility into the transaction
             */

            mtev_json_object *processor = mtev_json_object_object_get(children[i].data, "processor");
            if (!processor) {
              mtevL(tm_error, "Invalid elastic APM data, missing \"processor\" object trying to jaegerize\n");
              mtev_json_object_put(children[i].data);
              continue;
            }

            /* get the team of the trace member */
            const char *span_service_name = tm_service_name(children[i].data);
            team_data_t *other_td = NULL;
            if (tm_get_team(span_service_name, span_team)) {
              other_td = get_team_data(span_team);
            }

            if (other_td != td && rejaegered_root == false) {
              jaegerize_transaction(trans, other_td->jaeger_dest_url);
              rejaegered_root = true;
            }

            const char *event = mtev_json_object_get_string(mtev_json_object_object_get(processor, "event"));
            if (strcmp(event, "transaction") == 0) {
              jaegerize_transaction(children[i].data, td->jaeger_dest_url);
              if (other_td != td) {
                jaegerize_transaction(children[i].data, other_td->jaeger_dest_url);
              }
              jaegered_count++;
            } else if (strcmp(event, "span") == 0) {
              jaegerize_span(children[i].data, td->jaeger_dest_url);
              if (other_td != td) {
                jaegerize_span(children[i].data, other_td->jaeger_dest_url);
              }
              jaegered_count++;
            } else if (strcmp(event, "error") == 0) {
              jaegerize_error(children[i].data, td->jaeger_dest_url);
              if (other_td != td) {
                jaegerize_error(children[i].data, other_td->jaeger_dest_url);
              }
              jaegered_count++;
            }
            mtev_json_object_put(children[i].data);
          }
          free(children);
        }
      }
      mtev_json_object_put(json);
      s->trace = false;
      data.mv_data = s;
      data.mv_size = sizeof(lmdb_stored_root_t) + s->json_str_len;
      mdb_cursor_put(cursor, &key, &data, MDB_CURRENT);
      free(s);
      jaegered_count++;
      if (jaegered_count >= JAEGER_MAX_COUNT) break;
    }
  }

  mdb_cursor_close(cursor);
  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    mtevL(tm_error, "Error committing jaeger trans: %d\n", rc);
  }

  return jaegered_count;
}


void
tm_transaction_store_delete(const char *id, size_t id_len)
{
  int rc;
  MDB_txn *txn;
  MDB_val key, data;
  MDB_cursor_op op = MDB_SET_RANGE;
  MDB_cursor *cursor = NULL;

  rc = mdb_txn_begin(env, NULL, 0, &txn);
  if (rc != 0) {
    return;
  }

  rc = mdb_cursor_open(txn, dbi, &cursor);
  mtevAssert(rc == 0);

  key.mv_data = (void *)id;
  key.mv_size = id_len;
  while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
    op = MDB_NEXT;
    if (strncmp(id, (const char *)key.mv_data, id_len) != 0) {
      break;
    }

    mdb_cursor_del(cursor, 0);
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    mtevL(tm_error, "Could not commit deletion\n");
  }
}

void tm_transaction_store_entry_free(tm_transaction_store_entry_t *e)
{
  if (e == NULL) return;

  if (e->data) {
    pthread_mutex_lock(&lock);
    mtev_json_object_put(e->data);
    pthread_mutex_unlock(&lock);
  }
  free(e);
}
