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

#ifndef TM_TRANSACTION_STORE_H
#define TM_TRANSACTION_STORE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <mtev_json_object.h>

typedef struct tm_transaction_store_iter tm_transaction_store_iter_t;

typedef enum tm_transaction_store_type 
{
 TM_TRANSACTION_STORE_TYPE_MEMORY=0,
 TM_TRANSACTION_STORE_TYPE_LMDB=1
} tm_transaction_store_type;

typedef struct __attribute__ ((packed)) tm_transaction_store_entry {
  uint64_t first_seen_ms;
  uint64_t last_modified_ms;
  bool trace;
  mtev_json_object *data;
} tm_transaction_store_entry_t;

void tm_transaction_store_init(tm_transaction_store_type type, const char *path, size_t initial_size, int default_lookback_secs);
void tm_transaction_store_close();

/**
 * put an entry into the transaction store.  a ttl of zero will use the default lookback defined in the init call + 300 seconds
 *
 * TTL only governs delete age, lookback is used for distributed tracing
 */
bool tm_transaction_store_put(const char *id, size_t id_len, tm_transaction_store_entry_t *entry, int ttl);
bool tm_transaction_store_add_child(const char *trace_id, size_t id_len, mtev_json_object *child, int ttl);
size_t tm_transaction_store_get_children(const char *trace_id, size_t id_len, tm_transaction_store_entry_t **children);
tm_transaction_store_entry_t *tm_transaction_store_get(const char *id, size_t id_len);

/**
 * 
 *
 * returns jaeger'd count
 */
size_t tm_transaction_store_process_jaeger();

/* returns deleted count */
size_t tm_transaction_store_delete_old_transactions();
void tm_transaction_store_delete(const char *id, size_t id_len);

void tm_transaction_store_entry_free(tm_transaction_store_entry_t *e);

#endif
