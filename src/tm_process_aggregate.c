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

#include <stdint.h>
#include <mtev_json_object.h>

#include "tm_log.h"
#include "tm_metric.h"
#include "tm_process.h"
#include "tm_utils.h"

#include <math.h>

static bool
parse_key(const char *key, char *service_name, char *team)
{
  char tags_copy[3172] = {0};
  const char *tag_start = strstr(key, "|ST[");
  if (tag_start == NULL) {
    return false;
  }
  tag_start += 4;
  const char *tag_end = strstr(tag_start, "]");
  strncpy(tags_copy, tag_start, tag_end - tag_start);
  tags_copy[tag_end - tag_start] = '\0';

  char *tag = NULL, *src = tags_copy, *save1 = NULL;

  bool team_tag = false;
  while((tag = strtok_r(src, ",", &save1)) != NULL) {
    src = NULL;
    const char *service_start = strstr(tag, "service:");
    if (service_start) {
      strcpy(service_name, service_start + 8);
    }
    const char *team_start = strstr(tag, "team:");
    if (team_start) {
      strcpy(team, team_start + 5);
      team_tag = true;
    }
  }
  if (!team_tag) {
    tm_get_team(service_name, team);
  }
  return true;
}

static bool
_build_rollup_key(const char *incoming_key, const char *search, const char *replace, char *metric_name, size_t mlen)
{
  memset(metric_name, 0, mlen);

  const char *pos = strstr(incoming_key, search);
  if (pos) {
    strncpy(metric_name, incoming_key, pos - incoming_key);
    strlcat(metric_name, replace, mlen);
    strlcat(metric_name, pos + 9, mlen);
    return true;
  }
  return false;
}

static void
store_numeric_rollup(team_data_t *td, mtev_hash_table *team_metrics, const char *key, const char *replace, double value, uint64_t timestamp)
{
  /* skip invalid numerics */
  if (!isnormal(value)) return;

  char metric_name[4096];
  if (_build_rollup_key(key, "- latency", replace, metric_name, sizeof(metric_name))) {
    metric_key_t *mkey = make_metric_key(metric_name, timestamp);
    mkey->immediate_flush = false;
    void *existing = NULL;
    if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
      metric_value_t *v = (metric_value_t *)existing;
      v->last_seen = mtev_now_ms();
      ck_pr_store_double(&v->metric.number, value);
      metric_key_free(mkey);
    } else {
      pthread_mutex_lock(&td->mutex);
      if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
        metric_value_t *v = (metric_value_t *)existing;
        v->last_seen = mtev_now_ms();
        ck_pr_store_double(&v->metric.number, value);
        metric_key_free(mkey);
      } else {
        metric_value_t *v = make_numeric_value(value);
        mtev_hash_store(team_metrics, (const char *)mkey, mkey->key_len, v);
      }
      pthread_mutex_unlock(&td->mutex);
    }
  }
}

bool process_aggregate_message(topic_stats_t *stats, mtev_json_object *message)
{
  char service_name[256];
  char team[256];

  /* iterate the keys and skip, processor and context.. whatever is left is the data */
  mtev_json_object_object_foreach(message, key, val) {
    if (strcmp(key, "processor") == 0 || strcmp(key, "context") == 0) continue;

    if (parse_key(key, service_name, team)) {
      team_data_t *td = get_team_data(team);
      if (td == NULL) {
        mtevL(tm_debug, "Team not configured: %s\n", team);
        stats_add64(stats->unknown_team, 1);
        return false;
      }

      mtev_hash_table *team_metrics = &td->hash;

      /* val is a json metric
       *
       * "_type": "h",
       "_ts": 1553526300000,
       "_value": "AAEt/gAB"
      */
      mtev_json_object *type = mtev_json_object_object_get(val, "_type");
      const char *s = mtev_json_object_get_string(type);
      switch(*s) {
      case 'h':
        {
          /* inbound histogram
           *
           * deserialize the value and merge it with what we have so far
           */
          const char *b64_hist = mtev_json_object_get_string(mtev_json_object_object_get(val, "_value"));
          uint64_t timestamp = mtev_json_object_get_uint64(mtev_json_object_object_get(val, "_ts"));
          int owner = 0;
          mtev_json_object *o = mtev_json_object_object_get(val, "_owner");
          if (o) {
            owner = mtev_json_object_get_int(o);
          }

          histogram_t *h = hist_alloc();
          hist_deserialize_b64(h, b64_hist, strlen(b64_hist));

          double mean = 0.0;
          double min = 0.0;
          double max = 0.0;
          double stddev = 0.0;
          double ninefive = 0.0;
          double ninenine = 0.0;

          metric_key_t *mkey = make_metric_key(key, timestamp);
          mkey->immediate_flush = false;
          void *existing = NULL;
          if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
            metric_value_t *v = (metric_value_t *)existing;
            ck_spinlock_lock(&v->histogram_lock);
            ck_pr_store_64(&v->last_seen, mtev_now_ms());
            uint64_t existing_count = hist_sample_count(v->metric.hist);
            uint64_t incoming_count = hist_sample_count(h);
            if (existing_count > 35000 && existing_count < incoming_count && incoming_count - existing_count == 1) {
              mtevL(mtev_error, "Counts differ by one, key: %s, aggregate: %u, existing: %" PRIu64 ", incoming: %" PRIu64 ", owner: %d, incoming_owner: %d\n", 
                    mkey->metric_name, mkey->aggregate, existing_count, incoming_count, v->owner_id, owner);
            }

            hist_accumulate(v->metric.hist, (const histogram_t * const*)&h, 1);
            mean = hist_approx_mean(v->metric.hist);
            stddev = hist_approx_stddev(v->metric.hist);
            double p0 = 0.0;
            double p95 = 0.95;
            double p99 = 0.99;
            double p100 = 1.0;
            hist_approx_quantile(v->metric.hist, &p0, 1, &min);
            hist_approx_quantile(v->metric.hist, &p95, 1, &ninefive);
            hist_approx_quantile(v->metric.hist, &p99, 1, &ninenine);
            hist_approx_quantile(v->metric.hist, &p100, 1, &max);
            ck_spinlock_unlock(&v->histogram_lock);
            metric_key_free(mkey);
            hist_free(h);
          } else {
            /* take a lock at the team level and prevent data races */
            pthread_mutex_lock(&td->mutex);
            if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
              metric_value_t *v = (metric_value_t *)existing;
              ck_spinlock_lock(&v->histogram_lock);
              ck_pr_store_64(&v->last_seen, mtev_now_ms());
              hist_accumulate(v->metric.hist, (const histogram_t * const*)&h, 1);
              mean = hist_approx_mean(v->metric.hist);
              stddev = hist_approx_stddev(v->metric.hist);
              double p0 = 0.0;
              double p95 = 0.95;
              double p99 = 0.99;
              double p100 = 1.0;
              hist_approx_quantile(v->metric.hist, &p0, 1, &min);
              hist_approx_quantile(v->metric.hist, &p95, 1, &ninefive);
              hist_approx_quantile(v->metric.hist, &p99, 1, &ninenine);
              hist_approx_quantile(v->metric.hist, &p100, 1, &max);
              ck_spinlock_unlock(&v->histogram_lock);
              metric_key_free(mkey);
              hist_free(h);
            } else {
              metric_value_t *v = (metric_value_t *)mtev_memory_safe_malloc_cleanup(sizeof(metric_value_t), histogram_value_free);
              v->type = METRIC_VALUE_TYPE_HISTOGRAM;
              ck_pr_store_64(&v->last_seen, mtev_now_ms());
              v->flushed_ms = 0;
              v->owner_id = tm_metrics_get_owner_id();
              v->scheduled_for_destruction = false;
              v->metric.hist = h;
              mean = hist_approx_mean(v->metric.hist);
              stddev = hist_approx_stddev(v->metric.hist);
              double p0 = 0.0;
              double p95 = 0.95;
              double p99 = 0.99;
              double p100 = 1.0;
              hist_approx_quantile(v->metric.hist, &p0, 1, &min);
              hist_approx_quantile(v->metric.hist, &p95, 1, &ninefive);
              hist_approx_quantile(v->metric.hist, &p99, 1, &ninenine);
              hist_approx_quantile(v->metric.hist, &p100, 1, &max);
              ck_spinlock_init(&v->histogram_lock);
              mtev_hash_store(team_metrics, (const char *)mkey, mkey->key_len, v);
            }
            pthread_mutex_unlock(&td->mutex);
          }

          store_numeric_rollup(td, team_metrics, key, "- average_latency", mean, timestamp);
          store_numeric_rollup(td, team_metrics, key, "- min_latency", min, timestamp);
          store_numeric_rollup(td, team_metrics, key, "- p95_latency", ninefive, timestamp);
          store_numeric_rollup(td, team_metrics, key, "- p99_latency", ninenine, timestamp);
          store_numeric_rollup(td, team_metrics, key, "- max_latency", max, timestamp);
          store_numeric_rollup(td, team_metrics, key, "- stddev_latency", stddev, timestamp);
        }
        break;
      case 'L':
        {
          /* inbound integer (count)
           *
           * Add to our existing count
           */
          uint64_t count = mtev_json_object_get_uint64(mtev_json_object_object_get(val, "_value"));
          uint64_t timestamp = mtev_json_object_get_uint64(mtev_json_object_object_get(val, "_ts"));

          metric_key_t *mkey = make_metric_key(key, timestamp);
          mkey->immediate_flush = false;
          void *existing = NULL;
          bool debug_output = strncmp(mkey->metric_name, "transaction - request_count - all|ST[service:bdata-statsapi-analytics-prod_gke_us-east4_baseball,host:all,ip:all,method:GET]", mkey->key_len) == 0;
          if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
            if (debug_output) {
              mtevL(mtev_error, 
                    "METRIC_TS: %" PRIu64 ", inbound aggregate -- found in hash, adding: %" PRIu64 "\n", 
                    mkey->timestamp, count);
            }
            metric_value_t *v = (metric_value_t *)existing;
            ck_pr_store_64(&v->last_seen, mtev_now_ms());
            ck_pr_add_64(&v->metric.integer, count);
            metric_key_free(mkey);
          } else {
            /* take a lock at the team level and prevent data races */
            pthread_mutex_lock(&td->mutex);
            if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
              metric_value_t *v = (metric_value_t *)existing;
              ck_pr_store_64(&v->last_seen, mtev_now_ms());
              ck_pr_add_64(&v->metric.integer, count);
              metric_key_free(mkey);
            } else {
              if (debug_output) {
                mtevL(mtev_error, 
                      "METRIC_TS: %" PRIu64 ", inbound aggregate -- not in hash, adding: %" PRIu64 "\n", 
                      mkey->timestamp, count);
              }
              metric_value_t *v = make_integer_value(count);
              mtev_hash_store(team_metrics, (const char *)mkey, mkey->key_len, v);
            }
            pthread_mutex_unlock(&td->mutex);
          }
        }
        break;
      case 'n':
        {
          /* inbound numeric (average)
           */
          double num = mtev_json_object_get_double(mtev_json_object_object_get(val, "_value"));
          int count = mtev_json_object_get_int(mtev_json_object_object_get(val, "_count"));
          if (count == 0) count = 1;
          uint64_t timestamp = mtev_json_object_get_uint64(mtev_json_object_object_get(val, "_ts"));

          metric_key_t *mkey = make_metric_key(key, timestamp);
          mkey->immediate_flush = false;
          void *existing = NULL;
          if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
            metric_value_t *v = (metric_value_t *)existing;
            ck_pr_store_64(&v->last_seen, mtev_now_ms());
            ck_pr_add_double(&v->metric.number, num);
            ck_pr_add_int(&v->count, count);
            metric_key_free(mkey);
          } else {
            /* take a lock at the team level and prevent data races */
            pthread_mutex_lock(&td->mutex);
            if (mtev_hash_retrieve(team_metrics, (const char *)mkey, mkey->key_len, &existing)) {
              metric_value_t *v = (metric_value_t *)existing;
              ck_pr_store_64(&v->last_seen, mtev_now_ms());
              ck_pr_add_double(&v->metric.number, num);
              ck_pr_add_int(&v->count, count);
              metric_key_free(mkey);
            } else {
              metric_value_t *v = make_numeric_value(num);
              v->count = count;
              mtev_hash_store(team_metrics, (const char *)mkey, mkey->key_len, v);
            }
            pthread_mutex_unlock(&td->mutex);
          }
        }
        break;
      }
    }
  }

  return false;
}
