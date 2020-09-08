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

#ifndef TM_METRIC_H
#define TM_METRIC_H

#include <circllhist.h>
#include <mtev_hash.h>
#include <mtev_defines.h>
#include <mtev_json_object.h>
#include <mtev_memory.h>
#include <mtev_time.h>
#include <mtev_log.h>
#include <ctype.h>

#include "tm_kafka.h"


uint32_t tm_metrics_get_owner_id();

/* made up of:
 * 
 * - a timestamp in milliseconds since epoch
 * - the metric_name including tags
 */
typedef struct __attribute__((packed)) metric_key {
  uint64_t timestamp;
  int key_len;
  bool aggregate;
  bool immediate_flush;
  char metric_name[];
} metric_key_t;

enum METRIC_VALUE_TYPE
{
 METRIC_VALUE_TYPE_NUMBER=0,
 METRIC_VALUE_TYPE_TEXT=1,
 METRIC_VALUE_TYPE_HISTOGRAM=2,
 METRIC_VALUE_TYPE_INTEGER=3
};

typedef struct metric_value {
  enum METRIC_VALUE_TYPE type;
  union {
    double number;
    uint64_t integer;
    histogram_t *hist;
    char *str;
  } metric;
  int count;
  uint64_t last_seen;
  uint64_t flushed_ms;
  int32_t owner_id;
  bool scheduled_for_destruction;
  ck_spinlock_t histogram_lock;
} metric_value_t;

typedef struct apm_transaction_data {
  mtev_json_object *transaction;
  uint64_t first_seen_ms;
  bool trace;
  char url[];
} apm_transaction_data_t;

typedef struct pcre_matcher {
  pcre *match;
  pcre_extra *extra;
  char *replace;
} pcre_matcher;

typedef enum service_type
{
 SERVICE_TYPE_UNKNOWN=0,
 SERVICE_TYPE_TRANSACTIONAL=1,
 SERVICE_TYPE_MESSAGE=2,
 SERVICE_TYPE_SCHEDULED=3
} service_type_e;

typedef struct service_info {
  char *service_agent;
  service_type_e service_type;
  bool created;
} service_info_t;

typedef struct team_data {
  /* stores metrics where key is the circonus metric name */
  mtev_hash_table hash;
  char *team_name;
  char *circonus_api_key;
  char *check_id;
  /* stores hyperloglogs for each 5 minute window going back an hour */
  mtev_hash_table hlls;
  /* service name to td_path_squasher_t * */
  mtev_hash_table path_squashers;
  /* service name to set of regular expressions (regex string -> pcre_matcher struct) */
  mtev_hash_table squash_regexes;
  /* service name to info about the service */
  mtev_hash_table service_data;

  int path_squash_cardinality_factor;
  pthread_mutex_t mutex;
  char **allowlist;
  size_t allowlist_count;
  pcre_matcher **matcher;
  size_t matcher_count;
  char *metric_submission_url;
  char *jaeger_dest_url;
  char *transaction_db_path;
  uint64_t trace_threshold_us;
  mtev_boolean collect_host_level_metrics;
  mtev_boolean collect_host_level_system;
  mtev_boolean always_send_errors;
  mtev_boolean rollup_high_cardinality;
  uint64_t histogram_threshold_us;
  uint64_t transaction_lookback_secs;
  uint64_t apdex_satisfied_threshold_ms;
} team_data_t;


static inline metric_key_t *make_metric_key(const char *name, uint64_t timestamp_ms)
{
  size_t len = strlen(name);
  size_t key_len = sizeof(metric_key_t) + len + 1;
  metric_key_t *k = (metric_key_t *)mtev_memory_safe_malloc(key_len);
  k->timestamp = timestamp_ms;
  k->key_len = key_len;
  k->aggregate = false;
  k->immediate_flush = false;
  memcpy(k->metric_name, name, len + 1); // grab the NUL
  return k;
}

static inline metric_value_t *make_numeric_value(double d) {
  metric_value_t *v = (metric_value_t *)mtev_memory_safe_malloc(sizeof(metric_value_t));
  v->type = METRIC_VALUE_TYPE_NUMBER;
  v->metric.number = d;
  v->count = 1;
  v->last_seen = mtev_now_ms();
  v->flushed_ms = 0;
  v->scheduled_for_destruction = false;
  v->owner_id = tm_metrics_get_owner_id();
  return v;
}

static inline metric_value_t *make_integer_value(uint64_t i) {
  metric_value_t *v = (metric_value_t *)mtev_memory_safe_malloc(sizeof(metric_value_t));
  v->type = METRIC_VALUE_TYPE_INTEGER;
  v->metric.integer = i;
  v->count = 1;
  v->last_seen = mtev_now_ms();
  v->flushed_ms = 0;
  v->scheduled_for_destruction = false;
  v->owner_id = tm_metrics_get_owner_id();
  return v;
}

static void
histogram_value_free(void *x)
{
  metric_value_t *v = (metric_value_t *)x;
  hist_free(v->metric.hist);
}

static void
text_value_free(void *x)
{
  metric_value_t *v = (metric_value_t *)x;
  free(v->metric.str);
}

static inline metric_value_t *make_histogram_latency_value(const uint64_t duration_us) {
  metric_value_t *v = (metric_value_t *)mtev_memory_safe_malloc_cleanup(sizeof(metric_value_t), histogram_value_free);
  v->type = METRIC_VALUE_TYPE_HISTOGRAM;
  v->last_seen = mtev_now_ms();
  v->flushed_ms = 0;
  v->scheduled_for_destruction = false;
  v->owner_id = tm_metrics_get_owner_id();
  v->metric.hist = hist_alloc();
  v->count = 1;
  hist_insert_intscale(v->metric.hist, duration_us, -6, 1);
  ck_spinlock_init(&v->histogram_lock);
  return v;
}

static inline metric_value_t *make_text_value(const char *str) {
  metric_value_t *v = (metric_value_t *)mtev_memory_safe_malloc_cleanup(sizeof(metric_value_t), text_value_free);
  v->type = METRIC_VALUE_TYPE_TEXT;
  v->metric.str = strdup(str);
  v->count = 1;
  v->last_seen = mtev_now_ms();
  v->flushed_ms = 0;
  v->scheduled_for_destruction = false;
  v->owner_id = tm_metrics_get_owner_id();
  return v;
}


static inline void metric_key_free(void * v) {
  mtev_memory_safe_free(v);
}

static inline void metric_value_free(void * v) {
  mtev_memory_safe_free(v);
}

mtev_hash_table *get_team_metrics_table(const char *team);
team_data_t *get_team_data(const char *team);
void tm_add_team(const char *team, team_data_t *data);
void tm_metrics_init();
mtev_hash_table *get_all_team_data();

static inline void pcre_matcher_destroy(void *m) {
  pcre_matcher *match = (pcre_matcher *)m;
  pcre_free_study(match->extra);
  pcre_free(match->match);
  free(match->replace);
  free(match);
}

void tm_flush_team_metrics(mtev_hash_table *teams, tm_kafka_topic_t *agg_topic, tm_kafka_topic_t *regex_topic);

#endif
