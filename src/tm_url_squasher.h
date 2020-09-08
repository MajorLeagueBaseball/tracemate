#ifndef TM_URL_SQUASHER_H
#define TM_URL_SQUASHER_H

#include <mtev_hash.h>

#include <stdlib.h>
/**
 * A specialized tree that splits URL paths into their component segments while
 * squashing high cardinality segments into nothing.
 *
 * For example:
 *
 * /a/b/c/d
 *
 * Becomes a tree:
 *
 *               a
 *               |
 *               b
 *               |
 *               c
 *               |
 *               d
 *
 * Then when:
 *
 * /a/b/1234/e arrives the tree becomes:
 *
 *               a
 *               |
 *               b
 *              / \
 *            2134 c
 *             |   |
 *             e   d
 *
 * If `/a/b/c/d` arrives again, the tree is unchanged because the tree only
 * tracks unique paths.
 *
 * In this sense it's like a trie or patricia tree with the special property
 * that when any child list of a node eclipses a fixed threshold of cardinality
 * (set in constructor), that level is squashed. So paths of the family:
 *
 * /a/b/<integer>/e
 *
 * Would eventually be squashed into `/a/b/{integer}/e` and any newly matching
 * inbound paths of the same family would just be ignored as duplicates.
 *
 * The cardinality passed to the constructor is a cardinality "factor". The
 * first level of urls is always allowed otherwise we end up suppressing
 * everything. After that, the allowed cardinality of the level decays
 * exponentially as the levels get deeper and everything is chopped off at a max
 * of 5 levels.
 *
 * The eventual product of this tree is a list of regular expressions that will
 * match the inbound strings for all paths that had any component squashed.
 * 
 * The intention is to have a path_squasher per team that tracemate is
 * seeing and after that service has seen N (get_seen_count) URLs, you
 * could ask the path squasher for it's list of regexes.. These regexes
 * then get applied when making a URL generic (see tm_utils.c)
 */

typedef struct tm_path_squasher tm_path_squasher_t;

tm_path_squasher_t *tm_path_squasher_alloc(const int cardinality);
void tm_path_squasher_destroy(tm_path_squasher_t *ps);

void tm_path_squasher_add_path(tm_path_squasher_t *ps, const char *url_path);
int tm_path_squasher_get_seen_count(tm_path_squasher_t *ps);

/* map of regexstring -> pcre_matcher */
mtev_hash_table *tm_path_squasher_get_regexes(tm_path_squasher_t *ps);
void tm_path_squasher_print_tree(tm_path_squasher_t *ps);

#endif
