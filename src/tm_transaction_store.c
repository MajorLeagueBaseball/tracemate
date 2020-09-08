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
#include "bloom/bloom.h"

#include <mtev_compress.h>
#include <mtev_hash.h>

#include <ck_rwlock.h>
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

/* we have 2 environments and databases.
 *
 * one is the currently active db/env
 * and the other is the one we rotated out of
 *
 * When we have rotated out (every 1 hour), we:
 * - open a new "current"
 * - close "last"
 * - move old "current" -> "last"
 * - remove old "last" from disk
 *
 * Each db/env maintains a special dbentry: "tm_transaction_store_open_ms"
 * which contains the initial creation `mtev_now_ms()`
 * of that db/env.  This timestamp is used to calculate age
 * of the db/env.
 * 
 * Each env has an extra dbi to hold those records that have to be jaegered.
 *
 * When a db/env rotates to "last" it doesn't mean we don't continue
 * to write it.  Those pieces of traces that relate to stuff in
 * "last" still flow into last, but newly seen trace data flows into
 * "current".
 */
static uint64_t current_open_ms;
static MDB_env *current_env;
static MDB_dbi current_dbi;
static bloom_t *current_bloom;
static MDB_dbi current_jaeger_dbi;
static char *current_path;
static uint64_t last_open_ms;
static MDB_env *last_env;
static MDB_dbi last_dbi;
static MDB_dbi last_jaeger_dbi;
static bloom_t *last_bloom;
static char *last_path;
static size_t _initial_size;
static char *base_path;

static tm_transaction_store_type tm_type;
static ck_rwlock_t rwlock;
static int default_lookback_secs;

typedef struct stored {
  uint64_t first_seen_ms;
  bool trace;
  mtev_json_object *json;
  char data[];
} stored_t;

#define MAGIC_WORD_V2 0xd06f00d
#define MAGIC_WORD_V3 0xbadf00d
#define MAGIC_WORD_V4 0x0fa7a55
#define MAGIC_WORD_V4_COMPRESS 0x1fa7a55
typedef struct lmdb_stored_root_v4 {
  int magic_word;
  size_t json_str_len;
  uint64_t first_seen_ms;
  uint64_t last_modified_ms;
  int ttl;
  bool trace;
  char data[];
} __attribute__((packed)) lmdb_stored_root_t;

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
_tm_transaction_store_open_environments(size_t initial_size, const char *path, bool create_blooms)
{
  char subpath[1024];
  for (int i = 0; i < 2; i++) {
    MDB_env *env;
    MDB_dbi dbi, jaeger_dbi;
    uint64_t open_ms;
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

    if (i == 0) {
      snprintf(subpath, sizeof(subpath), "%s/current", path);
      current_path = strdup(subpath);
    } else {
      snprintf(subpath, sizeof(subpath), "%s/last", path);
      last_path = strdup(subpath);
    }
    rc = mdb_env_open(env, subpath, MDB_NORDAHEAD | MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOMEMINIT, 0644);
    mtevAssert(rc == 0);

    MDB_txn *txn;
    rc = mdb_txn_begin(env, NULL, 0, &txn);
    mtevAssert(rc == 0);
    rc = mdb_dbi_open(txn, "ts", MDB_CREATE, &dbi);
    mtevAssert(rc == 0);
    rc = mdb_dbi_open(txn, "jaeger", MDB_CREATE, &jaeger_dbi);
    mtevAssert(rc == 0);

    rc = mdb_txn_commit(txn);
    mtevAssert(rc == 0);

    /* get the creation TS from current (or create it) */
    MDB_val key, data;
    rc = mdb_txn_begin(env, NULL, 0, &txn);
    key.mv_data = "tm_transaction_store_open_ms";
    key.mv_size = strlen(key.mv_data);
    if (mdb_get(txn, dbi, &key, &data) == MDB_NOTFOUND) {
      data.mv_size = sizeof(uint64_t);
      open_ms = mtev_now_ms();
      data.mv_data = &open_ms;
      rc = mdb_put(txn, dbi, &key, &data, 0);
      mtevAssert(rc == 0);
    } else {
      open_ms = *(uint64_t*)data.mv_data;
    }
    rc = mdb_txn_commit(txn);
    mtevAssert(rc == 0);

    bloom_t *bloom = NULL;
    if (i == 0) {
      if (create_blooms) {
        bloom = current_bloom = bloom_create(50000000);
      }
      current_env = env;
      current_dbi = dbi;
      current_jaeger_dbi = jaeger_dbi;
      current_open_ms = open_ms;
    } else {
      if (create_blooms) {
        bloom = last_bloom = bloom_create(50000000);
      }
      last_env = env;
      last_dbi = dbi;
      last_jaeger_dbi = jaeger_dbi;
      last_open_ms = open_ms;
    }

    if (create_blooms) {
      /* populate the bloom filter */
      rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
      MDB_cursor *cursor;
      MDB_cursor_op op = MDB_FIRST;

      rc = mdb_cursor_open(txn, dbi, &cursor);
      mtevAssert(rc == 0);

      while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
        op = MDB_NEXT;
        bloom_add(bloom, key.mv_data, key.mv_size);
      }
      mdb_txn_abort(txn);
    }
  }
}

void
tm_transaction_store_init(tm_transaction_store_type type, const char *path,
                          size_t initial_size, int _default_lookback_secs)
{
  char subpath[1024];
  char temp[1024];
  tm_type = type;
  default_lookback_secs = _default_lookback_secs;
  base_path = strdup(path);
  _initial_size = initial_size;
  ck_rwlock_init(&rwlock);

  if (!_mkdir(path)) {
    mtevFatal(tm_error, "Could not create transaction store dir: %s\n", path);
  }

  snprintf(subpath, sizeof(subpath), "%s/current", path);
  if (!_mkdir(subpath)) {
    mtevFatal(tm_error, "Could not create transaction store dir: %s\n", subpath);
  }

  snprintf(subpath, sizeof(subpath), "%s/last", path);
  if (!_mkdir(subpath)) {
    mtevFatal(tm_error, "Could not create transaction store dir: %s\n", subpath);
  }

  /* open the current and last */
  _tm_transaction_store_open_environments(initial_size, path, true);

  /*
   * if there are current data.mdb and lock.mdb files in "path", copy them into "last"
   */
  snprintf(subpath, sizeof(subpath), "%s/data.mdb", path);
  if(access(subpath, F_OK) != -1) {

    uint64_t now = mtev_now_ms();
    MDB_env *env;
    MDB_dbi dbi;
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
    MDB_txn *insert_txn;
    rc = mdb_txn_begin(env, NULL, 0, &txn);
    mtevAssert(rc == 0);
    rc = mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
    mtevAssert(rc == 0);

    rc = mdb_txn_commit(txn);
    mtevAssert(rc == 0);

    /* now create a RDONLY txn so we can iterate the existing db */
    rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
    /* and create a read/write txn for inserting into "last" */
    rc = mdb_txn_begin(last_env, NULL, 0, &insert_txn);
    MDB_cursor *cursor;
    MDB_cursor_op op = MDB_FIRST;
    MDB_val key, data;

    rc = mdb_cursor_open(txn, dbi, &cursor);
    mtevAssert(rc == 0);

    int insert_count = 0;
    while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
      op = MDB_NEXT;
      lmdb_stored_root_t *ss = (lmdb_stored_root_t *)data.mv_data;
      rc = mdb_put(insert_txn, last_dbi, &key, &data, 0);
      mtevAssert(rc == 0);
      if (key.mv_size == 32 && ss->trace == true && rc == 0) {
        rc = mdb_put(insert_txn, last_jaeger_dbi, &key, &data, 0);
        mtevAssert(rc == 0);
      }
      insert_count++;
      if (insert_count % 100 == 0) {
        rc = mdb_txn_commit(insert_txn);
        mtevAssert(rc == 0);
        rc = mdb_txn_begin(last_env, NULL, 0, &insert_txn);
        mtevAssert(rc == 0);
      }
    }
    rc = mdb_txn_commit(insert_txn);
    mtevAssert(rc == 0);
    mdb_txn_abort(txn);

    uint64_t done = mtev_now_ms();
    mtevL(mtev_error, "Migration took: %" PRIu64 " ms\n", (done - now));
    /* remove the old files */
    mdb_dbi_close(env, dbi);
    mdb_env_close(env);

    // unlink the files
    snprintf(temp, sizeof(temp), "%s/data.mdb", path);
    unlink(temp);
    snprintf(temp, sizeof(temp), "%s/lock.mdb", path);
    unlink(temp);
  }
}

void
tm_transaction_store_close()
{
  mdb_dbi_close(current_env, current_dbi);
  mdb_env_close(current_env);
  mdb_dbi_close(last_env, last_dbi);
  mdb_env_close(last_env);
}

bool
tm_transaction_store_put(const char *id, size_t id_len, tm_transaction_store_entry_t *entry, int ttl)
{
  /* puts always go to current_env */
  MDB_val key, data;
  MDB_txn *txn;
  MDB_cursor *cursor;
  char *compressed = NULL;
  const char *store_string = NULL;
  size_t compressed_len = 0, store_len = 0;
  int rc;

  if (ttl == 0) ttl = default_lookback_secs + 300;

  const char *json_string = mtev_json_object_to_json_string(entry->data);
  size_t json_string_len = strlen(json_string);

  int mw = MAGIC_WORD_V4_COMPRESS;
  if (mtev_compress_lz4f(json_string, json_string_len, (unsigned char **)&compressed, &compressed_len) != 0) {
    store_string = json_string;
    store_len = json_string_len;
    mw = MAGIC_WORD_V4;
  } else {
    store_string = compressed;
    store_len = compressed_len;
  }

  size_t s_size = sizeof(lmdb_stored_root_t) + store_len;
  lmdb_stored_root_t *s = (lmdb_stored_root_t *)malloc(s_size);
  s->magic_word = mw;
  s->first_seen_ms = entry->first_seen_ms;
  s->last_modified_ms = mtev_now_ms();
  s->json_str_len = store_len;
  s->trace = entry->trace;
  s->ttl = ttl;
  memcpy(s->data, store_string, store_len);

  if (mw == MAGIC_WORD_V4_COMPRESS) {
    free(compressed);
  }

  key.mv_data = (void *)id;
  key.mv_size = id_len;

  data.mv_data = (void *)s;
  data.mv_size = s_size;

  ck_rwlock_read_lock(&rwlock);
  rc = mdb_txn_begin(current_env, NULL, 0, &txn);
  if (rc != 0) {
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }

  rc = mdb_cursor_open(txn, current_dbi, &cursor);
  if (rc != 0) {
    mdb_txn_abort(txn);
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }

  rc = mdb_cursor_put(cursor, &key, &data, 0);
  if (rc != 0) {
    mtevL(tm_error, "Failure to put %s, error: %s\n", id, mdb_strerror(rc));
    mdb_txn_abort(txn);
    free(s);
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }
  mdb_cursor_close(cursor);

  if (s->trace) {
    // if we are tracing, stick a copy in the jaeger database
    rc = mdb_cursor_open(txn, current_jaeger_dbi, &cursor);
    if (rc != 0) {
      mdb_txn_abort(txn);
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }

    rc = mdb_cursor_put(cursor, &key, &data, 0);
    if (rc != 0) {
      mtevL(tm_error, "Failure to put %s, error: %s\n", id, mdb_strerror(rc));
      mdb_txn_abort(txn);
      free(s);
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    mtevL(tm_error, "Failure to commit %s, error: %s\n", id, mdb_strerror(rc));
    free(s);
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }
  free(s);

  bloom_add(current_bloom, id, id_len);

  ck_rwlock_read_unlock(&rwlock);
  return true;
}

bool
tm_transaction_store_add_child(const char *trace_id, size_t id_len, mtev_json_object *child, int ttl)
{
  char root_key[96];
  size_t root_key_len = 0;
  MDB_val key, data;
  MDB_txn *txn = NULL;
  MDB_cursor *cursor = NULL;
  int rc;

  if (child == NULL) return false;

  if (ttl == 0) ttl = (default_lookback_secs + 300);

  key.mv_data = (void *)trace_id;
  key.mv_size = id_len;

  ck_rwlock_read_lock(&rwlock);

  /* check the bloom */
  if (!bloom_contains(last_bloom, trace_id, id_len)) {
    goto check_current;
  }

  // check the last_env first
  rc = mdb_txn_begin(last_env, NULL, 0, &txn);
  if (rc != 0) {
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }

  rc = mdb_cursor_open(txn, last_dbi, &cursor);
  if (rc != 0) {
    mdb_txn_abort(txn);
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }

  rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);
  if (rc != 0 && rc != MDB_NOTFOUND) {
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    ck_rwlock_read_unlock(&rwlock);
    return false;
  }

  if (rc == MDB_NOTFOUND) {
  check_current:

    /* check the bloom */
    if (!bloom_contains(current_bloom, trace_id, id_len)) {
      goto add_child;
    }

    // now check the current_env
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    rc = mdb_txn_begin(current_env, NULL, 0, &txn);

    rc = mdb_cursor_open(txn, current_dbi, &cursor);
    if (rc != 0) {
      mdb_txn_abort(txn);
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }

    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);
    if (rc != 0 && rc != MDB_NOTFOUND) {
      mdb_cursor_close(cursor);
      mdb_txn_abort(txn);
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }
  }

  if (rc == 0) {
    /* found the parent, alter last_modified_ms field */
    lmdb_stored_root_t *copy = NULL;
    lmdb_stored_root_t *s = (lmdb_stored_root_t *)data.mv_data;
    copy = (lmdb_stored_root_t *)malloc(sizeof(lmdb_stored_root_t) + s->json_str_len);
    memcpy(copy, s, data.mv_size);

    copy->last_modified_ms = mtev_now_ms();
    data.mv_data = copy;
    data.mv_size = sizeof(lmdb_stored_root_t) + copy->json_str_len;
    rc = mdb_cursor_put(cursor, &key, &data, 0);
    if (rc != 0) {
      mtevL(tm_error, "Failure to put %s, error: %s\n", trace_id, mdb_strerror(rc));
      mdb_txn_abort(txn);
      free(copy);
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }
    free(copy);
  }
 add_child:
  {

    if (txn == NULL) {
      rc = mdb_txn_begin(current_env, NULL, 0, &txn);

      rc = mdb_cursor_open(txn, current_dbi, &cursor);
      if (rc != 0) {
        mdb_txn_abort(txn);
        ck_rwlock_read_unlock(&rwlock);
        return false;
      }
    }

    char *compressed = NULL;
    const char *store_string = NULL;
    size_t compressed_len = 0, store_len = 0;
    const char *json_string = mtev_json_object_to_json_string(child);
    size_t json_string_len = strlen(json_string);

    int mw = MAGIC_WORD_V4_COMPRESS;
    if (mtev_compress_lz4f(json_string, json_string_len, (unsigned char **)&compressed, &compressed_len) != 0) {
      store_string = json_string;
      store_len = json_string_len;
      mw = MAGIC_WORD_V4;
    } else {
      store_string = compressed;
      store_len = compressed_len;
    }

    size_t s_size = sizeof(lmdb_stored_child_t) + store_len;
    lmdb_stored_child_t *sc = (lmdb_stored_child_t *)malloc(s_size);
    sc->magic_word = mw;
    sc->first_seen_ms = mtev_now_ms();
    sc->json_str_len = store_len;
    sc->ttl = ttl;
    memcpy(sc->data, store_string, store_len);

    if (mw == MAGIC_WORD_V4_COMPRESS) {
      free(compressed);
    }

    mtev_json_object *processor = mtev_json_object_object_get(child, "processor");
    mtev_json_object *event = mtev_json_object_object_get(processor, "event");
    if (event) {
      const char *ev = mtev_json_object_get_string(event);
      const char *hex_id = NULL;
      if (strcmp(ev, "span") == 0) {
        mtev_json_object *span = mtev_json_object_object_get(child, "span");
        mtev_json_object *id = mtev_json_object_object_get(span, "hex_id");
        hex_id = mtev_json_object_get_string(id);
      }
      else if (strcmp(ev, "error") == 0) {
        mtev_json_object *error = mtev_json_object_object_get(child, "error");
        mtev_json_object *id = mtev_json_object_object_get(error, "id");
        hex_id = mtev_json_object_get_string(id);
      }
      else if (strcmp(ev, "transaction") == 0) {
        mtev_json_object *trans = mtev_json_object_object_get(child, "transaction");
        mtev_json_object *id = mtev_json_object_object_get(trans, "id");
        hex_id = mtev_json_object_get_string(id);
      }
      root_key_len = snprintf(root_key, sizeof(root_key), "%.*s-%s", (int)id_len, trace_id, hex_id);
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
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }
    mdb_cursor_close(cursor);
    rc = mdb_txn_commit(txn);
    if (rc != 0) {
      mtevL(tm_error, "Failure to commit %s, error: %s\n", trace_id, mdb_strerror(rc));
      free(sc);
      ck_rwlock_read_unlock(&rwlock);
      return false;
    }
    free(sc);
    ck_rwlock_read_unlock(&rwlock);
    return true;
  }
}


tm_transaction_store_entry_t *
tm_transaction_store_get(const char *id, size_t id_len)
{
  MDB_val key, data;
  MDB_txn *txn;
  MDB_cursor *cursor;
  int rc;

  ck_rwlock_read_lock(&rwlock);

  /* check the last bloom */
  if (!bloom_contains(last_bloom, id, id_len)) {
    goto check_current;
  }

  // check last_env first
  rc = mdb_txn_begin(last_env, NULL, MDB_RDONLY, &txn);
  mtevAssert(rc == 0);

  rc = mdb_cursor_open(txn, last_dbi, &cursor);
  mtevAssert(rc == 0);

  key.mv_data = (void *)id;
  key.mv_size = id_len;
  rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);
  if (rc != 0) {
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

  check_current:
    /* check the current bloom */
    if (!bloom_contains(current_bloom, id, id_len)) {
      ck_rwlock_read_unlock(&rwlock);
      return NULL;
    }

    rc = mdb_txn_begin(current_env, NULL, MDB_RDONLY, &txn);
    mtevAssert(rc == 0);

    rc = mdb_cursor_open(txn, current_dbi, &cursor);
    mtevAssert(rc == 0);

    key.mv_data = (void *)id;
    key.mv_size = id_len;
    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_KEY);

    if (rc != 0) {
      mdb_cursor_close(cursor);
      mdb_txn_abort(txn);
      ck_rwlock_read_unlock(&rwlock);
      return NULL;
    }
  }

  tm_transaction_store_entry_t *rval = NULL;
  lmdb_stored_root_t *s = (lmdb_stored_root_t *)data.mv_data;
  const char *json = NULL;
  size_t json_len = 0;

  /* we have to copy because the calling process treats this as a mutable object */
  rval = (tm_transaction_store_entry_t *)calloc(1, sizeof(tm_transaction_store_entry_t));

  if (s->magic_word == MAGIC_WORD_V4_COMPRESS) {
    size_t in_size = s->json_str_len, out_size = mtev_compress_bound(MTEV_COMPRESS_LZ4F, s->json_str_len);
    unsigned char *decompressed = NULL;
    decompressed = malloc(out_size);

    mtev_stream_decompress_ctx_t *decomp = mtev_create_stream_decompress_ctx();
    mtev_stream_decompress_init(decomp, MTEV_COMPRESS_LZ4F);
    int drc = mtev_stream_decompress(decomp, (const unsigned char *)s->data, &in_size, decompressed, &out_size);
    mtev_stream_decompress_finish(decomp);
    mtev_destroy_stream_decompress_ctx(decomp);
    if (drc != 0) {
      mtevL(mtev_error, "Cannot decompress entry: %d\n", drc);
      mdb_cursor_close(cursor);
      mdb_txn_abort(txn);
      ck_rwlock_read_unlock(&rwlock);
      free(decompressed);
      return NULL;
    }
    json_len = out_size;
    json = (const char *)decompressed;
  } else {
    json = (const char *)s->data;
    json_len = s->json_str_len;
  }

  mtev_json_tokener *tok = mtev_json_tokener_new();
  mtev_json_object *json_o = mtev_json_tokener_parse_ex(tok, json, json_len);
  mtev_json_tokener_free(tok);

  if (s->magic_word == MAGIC_WORD_V4_COMPRESS) {
    free((char *)json);
  }

  rval->first_seen_ms = s->first_seen_ms;
  rval->last_modified_ms = s->last_modified_ms;
  rval->trace = s->trace;
  rval->data = json_o;
  mdb_cursor_close(cursor);
  mdb_txn_abort(txn);
  ck_rwlock_read_unlock(&rwlock);
  return rval;
}

static size_t
_tm_transaction_store_get_children(MDB_txn *txn, MDB_dbi database, const char *id, size_t id_len, tm_transaction_store_entry_t **children)
{
  MDB_val key, data;
  MDB_cursor *cursor;
  MDB_cursor_op op = MDB_SET_RANGE;
  int rc;

  rc = mdb_cursor_open(txn, database, &cursor);
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

    const char *json = NULL;
    size_t json_len = 0;
    int *mw = (int *)data.mv_data;
    mtev_json_object *json_o = NULL;
    lmdb_stored_child_t *s = (lmdb_stored_child_t *)data.mv_data;

    if (*mw == MAGIC_WORD_V4_COMPRESS) {
      size_t in_size = s->json_str_len, out_size = mtev_compress_bound(MTEV_COMPRESS_LZ4F, s->json_str_len);
      unsigned char *decompressed = NULL;
      decompressed = malloc(out_size);

      mtev_stream_decompress_ctx_t *decomp = mtev_create_stream_decompress_ctx();
      mtev_stream_decompress_init(decomp, MTEV_COMPRESS_LZ4F);
      int drc = mtev_stream_decompress(decomp, (const unsigned char *)s->data, &in_size, decompressed, &out_size);
      mtev_stream_decompress_finish(decomp);
      mtev_destroy_stream_decompress_ctx(decomp);

      if (drc != 0) {
        mtevL(mtev_error, "Cannot decompress entry: %d\n", drc);
        mdb_cursor_close(cursor);
        free(decompressed);
        return 0;
      }
      json_len = out_size;
      json = (const char *)decompressed;
    } else {
      json = (const char *)s->data;
      json_len = s->json_str_len;
    }

    mtev_json_tokener *tok = mtev_json_tokener_new();
    json_o = mtev_json_tokener_parse_ex(tok, json, json_len);
    mtev_json_tokener_free(tok);

    if (*mw == MAGIC_WORD_V4_COMPRESS) {
      free((char *)json);
    }

    if (json_o != NULL) {
      (*children)[child_count].data = json_o;
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

  ck_rwlock_read_lock(&rwlock);
  rc = mdb_txn_begin(current_env, NULL, MDB_RDONLY, &txn);
  mtevAssert(rc == 0);

  size_t c = _tm_transaction_store_get_children(txn, current_dbi, id, id_len, children);
  mdb_txn_abort(txn);
  ck_rwlock_read_unlock(&rwlock);
  return c;
}

#define ONE_HOURS_MS (1 * 60 * 60 * 1000)

size_t
tm_transaction_store_delete_old_transactions()
{
  char path[1024];
  char temp[1024];
  uint64_t now = mtev_now_ms();

  // first see if we are doing the flip
  if (now > current_open_ms && now - current_open_ms > ONE_HOURS_MS) {
    // we are doing the flip
    ck_rwlock_write_lock(&rwlock);

    MDB_stat s;
    MDB_txn *txn;
    int rc = mdb_txn_begin(current_env, NULL, MDB_RDONLY, &txn);
    mtevAssert(rc == 0);
    mdb_stat(txn, current_dbi, &s);
    mdb_txn_abort(txn);

    // close last_env and current_env
    tm_transaction_store_close();

    // rotate bloom filters
    bloom_t *to_destroy = last_bloom;
    last_bloom = current_bloom;
    current_bloom = bloom_create(50000000);
    bloom_destroy(to_destroy);

    // unlink the files and move current->last
    snprintf(path, sizeof(path), "%s/data.mdb", last_path);
    unlink(path);
    snprintf(temp, sizeof(temp), "%s/data.mdb", current_path);
    rename(temp, path);

    snprintf(path, sizeof(path), "%s/lock.mdb", last_path);
    unlink(path);
    snprintf(temp, sizeof(temp), "%s/lock.mdb", current_path);
    rename(temp, path);

    // reopen
    _tm_transaction_store_open_environments(_initial_size, base_path, false);
    uint64_t done = mtev_now_ms();
    mtevL(mtev_error, "Clean took: %" PRIu64 " ms\n", (done - now));
    ck_rwlock_write_unlock(&rwlock);
    return s.ms_entries;
  }
  return 0;
}

size_t
tm_transaction_store_process_jaeger()
{
#define JAEGER_MAX_COUNT 4000
  int rc;
  size_t jaegered_count = 0;
  MDB_txn *txn;
  MDB_cursor *cursor;
  MDB_val key, data;
  MDB_cursor_op op = MDB_FIRST;
  uint64_t time_ms = mtev_now_ms() - (uint64_t)(default_lookback_secs * 1000UL);
  uint64_t now = mtev_now_ms();

  ck_rwlock_read_lock(&rwlock);
  for (int i = 0; i < 2; i++) {
    MDB_env *env = i == 0 ? current_env : last_env;
    MDB_dbi dbi = i == 0 ? current_jaeger_dbi : last_jaeger_dbi;
    MDB_dbi parent_dbi = i == 0 ? current_dbi : last_dbi;

    rc = mdb_txn_begin(env, NULL, 0, &txn);
    if (rc != 0) {
      ck_rwlock_read_unlock(&rwlock);
      return 0;
    }

    rc = mdb_cursor_open(txn, dbi, &cursor);
    if (rc != 0) {
      mdb_txn_abort(txn);
      ck_rwlock_read_unlock(&rwlock);
      return 0;
    }

    char root_span_team[128];
    char span_team[128];
    while ((rc = mdb_cursor_get(cursor, &key, &data, op)) == 0) {
      op = MDB_NEXT;

      mtev_json_object *json_o = NULL;
      lmdb_stored_root_t *ss = (lmdb_stored_root_t *)data.mv_data;
      if (ss->last_modified_ms <= time_ms && ss->trace == true) {

        const char *json = NULL;
        size_t json_len = 0;

        if (ss->magic_word == MAGIC_WORD_V4_COMPRESS) {
          size_t in_size = ss->json_str_len, out_size = mtev_compress_bound(MTEV_COMPRESS_LZ4F, ss->json_str_len);
          unsigned char *decompressed = NULL;
          decompressed = malloc(out_size);

          mtev_stream_decompress_ctx_t *decomp = mtev_create_stream_decompress_ctx();
          mtev_stream_decompress_init(decomp, MTEV_COMPRESS_LZ4F);
          int drc = mtev_stream_decompress(decomp, (const unsigned char *)ss->data, &in_size, decompressed, &out_size);
          mtev_stream_decompress_finish(decomp);
          mtev_destroy_stream_decompress_ctx(decomp);
          if (drc != 0) {
            mtevL(mtev_error, "Cannot decompress entry: %d\n", drc);
            free(decompressed);
            continue;
          }
          json_len = out_size;
          json = (const char *)decompressed;
        } else {
          json = (const char *)ss->data;
          json_len = ss->json_str_len;
        }

        mtev_json_tokener *tok = mtev_json_tokener_new();
        json_o = mtev_json_tokener_parse_ex(tok, json, json_len);
        mtev_json_tokener_free(tok);
        if (ss->magic_word == MAGIC_WORD_V4_COMPRESS) {
          free((char *)json);
        }
      }
      if (json_o != NULL) {
        mtev_json_object *trans = mtev_json_object_object_get(json_o, "transaction");
        if (trans) {
          bool rejaegered_root = false;
          const char *service_name = tm_service_name(trans);
          if (tm_get_team(service_name, root_span_team)) {
            team_data_t *td = get_team_data(root_span_team);
            jaegerize_transaction(trans, td->jaeger_dest_url);
            jaegered_count++;

            tm_transaction_store_entry_t *children = NULL;
            size_t child_count = _tm_transaction_store_get_children(txn, parent_dbi, key.mv_data, key.mv_size, &children);
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
        mtev_json_object_put(json_o);
        mdb_cursor_del(cursor, 0);
        jaegered_count++;
        if (jaegered_count >= JAEGER_MAX_COUNT) break;
      }
    }

    mdb_cursor_close(cursor);
    rc = mdb_txn_commit(txn);
    if (rc != 0) {
      mtevL(tm_error, "Error committing jaeger trans: %d\n", rc);
    }
  }
  ck_rwlock_read_unlock(&rwlock);
  uint64_t done = mtev_now_ms();
  mtevL(mtev_error, "Jaeger took: %" PRIu64 " ms\n", (done - now));
  return jaegered_count;
}


void tm_transaction_store_entry_free(tm_transaction_store_entry_t *e)
{
  if (e == NULL) return;

  if (e->data) {
    ck_rwlock_read_lock(&rwlock);
    mtev_json_object_put(e->data);
    ck_rwlock_read_unlock(&rwlock);
  }
  free(e);
}
