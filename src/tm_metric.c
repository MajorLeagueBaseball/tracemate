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

#include "tm_circonus.h"
#include "tm_metric.h"
#include "tm_utils.h"
#include "tm_log.h"
#include "tm_kafka.h"

#include <mtev_dyn_buffer.h>
#include <mtev_rand.h>

#define TWO_MB 2097152
static int32_t owner_id = 0;

/* team name to of team settings and metrics hash of metrics */
static mtev_hash_table team_metrics;

void tm_metrics_init() {
  mtev_hash_init(&team_metrics);
  owner_id = mtev_rand() >> 32;
}

uint32_t tm_metrics_get_owner_id() {
  return owner_id;
}

mtev_hash_table *get_team_metrics_table(const char *team) {
  team_data_t *td = get_team_data(team);
  if (td) return &td->hash;
  return NULL;
}

team_data_t *get_team_data(const char *team) {
  void *hash = NULL;
  if (mtev_hash_retrieve(&team_metrics, team, strlen(team), &hash)) {
    team_data_t *d = (team_data_t *)hash;
    return d;
  }
  return NULL;
}

void tm_add_team(const char *team, team_data_t *data) {
  char *copy = strdup(team);
  pthread_mutex_init(&data->mutex, NULL);
  mtev_hash_init_mtev_memory(&data->hash, 1000, MTEV_HASH_LOCK_MODE_MUTEX);
  mtev_hash_store(&team_metrics, copy, strlen(copy), data);
}

mtev_hash_table *get_all_team_data()
{
  return &team_metrics;
}

static const char *json_hex_chars = "0123456789abcdef";

static int json_escape_str(mtev_dyn_buffer_t *pb, char *str)
{
  int pos = 0, start_offset = 0;
  unsigned char c;
  do {
    c = str[pos];
    switch(c) {
    case '\0':
      break;
    case '\b':
    case '\n':
    case '\r':
    case '\t':
    case '"':
    case '\\':
    case '/':
      if(pos - start_offset > 0) {
        mtev_dyn_buffer_add(pb, (uint8_t *)str + start_offset, pos - start_offset);
      }
      if(c == '\b') mtev_dyn_buffer_add(pb, (uint8_t *)"\\b", 2);
      else if(c == '\n') mtev_dyn_buffer_add(pb, (uint8_t *)"\\n", 2);
      else if(c == '\r') mtev_dyn_buffer_add(pb, (uint8_t *)"\\r", 2);
      else if(c == '\t') mtev_dyn_buffer_add(pb, (uint8_t *)"\\t", 2);
      else if(c == '"') mtev_dyn_buffer_add(pb, (uint8_t *)"\\\"", 2);
      else if(c == '\\') mtev_dyn_buffer_add(pb, (uint8_t *)"\\\\", 2);
      else if(c == '/') mtev_dyn_buffer_add(pb, (uint8_t *)"\\/", 2);
      start_offset = ++pos;
      break;
    default:
      if(c < ' ') {
        if(pos - start_offset > 0) {
          mtev_dyn_buffer_add(pb, (uint8_t *)str + start_offset, pos - start_offset);
        }
        mtev_dyn_buffer_add_printf(pb, "\\u00%c%c",
               json_hex_chars[c >> 4],
               json_hex_chars[c & 0xf]);
        start_offset = ++pos;
      } else pos++;
    }
  } while(c);
  if(pos - start_offset > 0) {
    mtev_dyn_buffer_add(pb, (uint8_t *)str + start_offset, pos - start_offset);
  }
  return 0;
}


static void
metric_to_json(mtev_dyn_buffer_t *json, metric_key_t *k, metric_value_t *v, bool resolve_avg, bool reset)
{
  /* reserve space for stuff */
  mtev_dyn_buffer_ensure(json, 4096);
  mtev_dyn_buffer_add(json, (uint8_t *)"\"", 1);
  /* escape the metric_name string just in case weird chars have gotten in */
  json_escape_str(json, k->metric_name);

  mtev_dyn_buffer_add(json, (uint8_t *)"\":{", 3);

  switch(v->type) {
  case METRIC_VALUE_TYPE_NUMBER:
    {
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_type\":\"n\",", 12);
      mtev_dyn_buffer_add_printf(json, "\"_owner\": %u,", v->owner_id);
      if (resolve_avg == false) {
        mtev_dyn_buffer_add_printf(json, "\"_count\": %d,", v->count);
        mtev_dyn_buffer_add_printf(json, "\"_value\": %g,", v->metric.number);
      } else {
        mtev_dyn_buffer_add_printf(json, "\"_value\": %g,", v->metric.number / MAX((double)v->count, 1.0));
      }
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu", k->timestamp);
      if (reset) {
        ck_pr_store_int(&v->count, 0);
        ck_pr_store_double(&v->metric.number, 0.0);
      }
    }
    break;
  case METRIC_VALUE_TYPE_TEXT:
    {
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_type\":\"s\",", 12);
      mtev_dyn_buffer_add_printf(json, "\"_owner\": %u,", v->owner_id);
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_value\": \"", 11);
      json_escape_str(json, v->metric.str);
      mtev_dyn_buffer_add(json, (uint8_t *)"\",", 2);
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu", k->timestamp);
    }
    break;
  case METRIC_VALUE_TYPE_INTEGER:
    {
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_type\":\"L\",", 12);
      mtev_dyn_buffer_add_printf(json, "\"_value\": %" PRIu64 ",", v->metric.integer);
      mtev_dyn_buffer_add_printf(json, "\"_owner\": %u,", v->owner_id);
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu", k->timestamp);
      if (reset) {
        ck_pr_store_64(&v->metric.integer, 0);
      }
    }
    break;
  case METRIC_VALUE_TYPE_HISTOGRAM:
    {
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_type\":\"h\",", 12);
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu,", k->timestamp);
      mtev_dyn_buffer_add_printf(json, "\"_owner\": %u,", v->owner_id);
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_value\":\"", 10);
      ck_spinlock_lock(&v->histogram_lock);
      histogram_t *h = v->metric.hist;
      ssize_t est = hist_serialize_b64_estimate(h);
      mtev_dyn_buffer_ensure(json, est + 1);
      ssize_t actual = hist_serialize_b64(h, (char *)mtev_dyn_buffer_write_pointer(json), est);
      if (reset) {
        hist_clear(h);
      }
      ck_spinlock_unlock(&v->histogram_lock);

      mtevAssert(actual <= est);
      mtev_dyn_buffer_advance(json, actual);
      mtev_dyn_buffer_add(json, (uint8_t *)"\"", 1);
    }
    break;
  };
  mtev_dyn_buffer_add(json, (uint8_t *)"}", 1);
}

static bool
send_aggregate_to_kafka(mtev_dyn_buffer_t *json, const char *key, size_t key_len, team_data_t *td, tm_kafka_topic_t *agg_topic)
{
#ifdef NO_PUBLISH
  mtevL(mtev_error, "Publishing off, not sending to kafka: %.*s\n", (int)mtev_dyn_buffer_used(json), mtev_dyn_buffer_data(json));
  return true;
#else
  return tm_kafka_produce(agg_topic, mtev_dyn_buffer_data(json), mtev_dyn_buffer_used(json), key, key_len);
#endif
}

void tm_flush_team_metrics(mtev_hash_table *teams, tm_kafka_topic_t *agg_topic)
{
  uint64_t now = mtev_now_ms();
  mtev_dyn_buffer_t json;
  mtev_dyn_buffer_t kafka;
  mtev_dyn_buffer_init(&json);
  mtev_dyn_buffer_init(&kafka);

  mtev_hash_iter it = MTEV_HASH_ITER_ZERO;
  while(mtev_hash_adv(teams, &it)) {
    team_data_t *td = (team_data_t *)it.value.ptr;
    const char *name = it.key.str;
    mtevL(tm_notice, "Flushing team: %s\n", name);

    /**
     * We want to make one JSON post per team here.. so walk all the flushable metrics
     * and build up json as we go
     */
    mtev_dyn_buffer_reset(&json);
    mtev_dyn_buffer_add(&json, (uint8_t *)"{", 1);
    bool comma = false;

    size_t original_hash_size = mtev_hash_size(&td->hash);
    metric_value_t **json_expired_list = (metric_value_t **)calloc(original_hash_size, sizeof(metric_value_t *));
    size_t json_expired_count = 0;
    metric_key_t **kafka_expired_list = (metric_key_t **)calloc(original_hash_size, sizeof(metric_key_t *));
    size_t kafka_expired_count = 0;

    /* iterate our list of keys, find expired, build json and send to metric endpoint,
     * or build json and send back to kafka for aggregation
     */
    mtev_hash_iter metric = MTEV_HASH_ITER_ZERO;
    while(mtev_hash_adv_spmc(&td->hash, &metric)) {
      metric_key_t *k = (metric_key_t *)metric.key.ptr;
      metric_value_t *v = (metric_value_t *)metric.value.ptr;

      /* delete when the flushed_ms >= last_seen.
       * 
       * The idea here is that we do an immediate flush but straggler data might come in 
       * and make the numbers more accurate over time, so we reflush with the updated numbers
       * but if nothing new came in for a while (10 minutes) we can delete it 
       */
      if (v->flushed_ms > 0 && v->flushed_ms >= v->last_seen && now > v->last_seen && now - v->last_seen > 600000) {
        v->scheduled_for_destruction = true;
        kafka_expired_list[kafka_expired_count] = k;
        kafka_expired_count++;
        continue;
      }

      /* otherwise, if we have already flushed this but it hasn't been 10 minutes, move along */
      if (v->flushed_ms > 0 && v->flushed_ms >= v->last_seen) continue;


      if (k->aggregate == false) {
        if (k->immediate_flush || ((now > k->timestamp && now - k->timestamp > 1000) && (now > v->last_seen && now - v->last_seen > 10000))) {
          if (comma) {
            mtev_dyn_buffer_add(&json, (uint8_t *)",", 1);
          }
          comma = true;

          /* place the keys that we need to flush in a list for later deletion */
          json_expired_list[json_expired_count] = v;
          json_expired_count++;

          metric_to_json(&json, k, v, true, false);
        }
      }
      else {
        /* for aggregates, send to kafka topic after a very short delay, they will be agged on the other side */
        if ((now > v->last_seen && now - v->last_seen > 1000)) {
          mtev_dyn_buffer_reset(&kafka);
          mtev_dyn_buffer_add(&kafka, (uint8_t *)"{", 1);
          /* fake the APM fields */
          mtev_dyn_buffer_ensure(&kafka, 1024);
          mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{\"%s\": \"%s\"},", "processor", "event", "aggregate");
          mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{},", "context");

          /* build json */
          metric_to_json(&kafka, k, v, false, true);
          v->scheduled_for_destruction = true;
          kafka_expired_list[kafka_expired_count] = k;
          kafka_expired_count++;

          mtev_dyn_buffer_add(&kafka, (uint8_t *)"}", 1);
          if (send_aggregate_to_kafka(&kafka, k->metric_name, strlen(k->metric_name), td, agg_topic)) {
            /* place the keys that we need to flush in a list for later deletion */
            uint64_t flushed_ms = mtev_now_ms();
            v->flushed_ms = flushed_ms;
          }
        }
      }

      if (mtev_dyn_buffer_used(&json) > TWO_MB) {
        mtev_dyn_buffer_add(&json, (uint8_t *)"}", 1);
        if (journal_output_metrics(&json, json_expired_count, td->metric_submission_url)) {
          uint64_t flushed_ms = mtev_now_ms();
          /* iterate our list of json keys and mark as flushed */
          for (size_t i = 0; i < json_expired_count; i++) {
            metric_value_t *v = json_expired_list[i];
            if (v != NULL) {
              v->flushed_ms = flushed_ms;
            }
          }
          mtev_dyn_buffer_reset(&json);
          mtev_dyn_buffer_add(&json, (uint8_t *)"{", 1);
          json_expired_count = 0;
          comma = false;
        } else {
          goto next_team;
        }
      }
    }
    mtev_dyn_buffer_add(&json, (uint8_t *)"}", 1);

    if (journal_output_metrics(&json, json_expired_count, td->metric_submission_url)) {

      uint64_t flushed_ms = mtev_now_ms();
      /* iterate our list of json keys and mark as flushed */
      for (size_t i = 0; i < json_expired_count; i++) {
        metric_value_t *v = json_expired_list[i];
        if (v != NULL) {
          v->flushed_ms = flushed_ms;
        }
      }
    }

  next_team:
    /* iterate our list of kafka keys and remove from hash */
    for (size_t i = 0; i < kafka_expired_count; i++) {
      metric_key_t *k = kafka_expired_list[i];
      if (k != NULL) {
        pthread_mutex_lock(&td->mutex);
        mtev_hash_delete(&td->hash, (const char *)k, k->key_len, metric_key_free, metric_value_free);
        pthread_mutex_unlock(&td->mutex);
      }
    }

    free(json_expired_list);
    free(kafka_expired_list);
    json_expired_count = 0;
    kafka_expired_count = 0;
  }
  mtev_dyn_buffer_destroy(&json);
  mtev_dyn_buffer_destroy(&kafka);
}
