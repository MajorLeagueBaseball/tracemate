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
#include "tm_process.h"
#include "tm_url_squasher.h"

#include <mtev_dyn_buffer.h>
#include <mtev_hyperloglog.h>
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

  team_data_t *new_team = (team_data_t *)calloc(1, sizeof(team_data_t));
  new_team->metric_submission_url = strdup("https://api.circonus.com/module/httptrap/8e9570ba-1448-41a4-9aef-4c2b85467a11/V8w7Upqp");
  new_team->jaeger_dest_url = strdup("apm-collector-headless.o11y:14250");
  tm_add_team(team, new_team);
  return new_team;
}

void tm_add_team(const char *team, team_data_t *data) {
  char *copy = strdup(team);
  data->team_name = copy;
  pthread_mutex_init(&data->mutex, NULL);
  mtev_hash_init_mtev_memory(&data->hash, 1000, MTEV_HASH_LOCK_MODE_MUTEX);
  mtev_hash_init_mtev_memory(&data->hlls, 20, MTEV_HASH_LOCK_MODE_NONE);
  mtev_hash_init_mtev_memory(&data->path_squashers, 20, MTEV_HASH_LOCK_MODE_NONE);
  mtev_hash_init_mtev_memory(&data->squash_regexes, 20, MTEV_HASH_LOCK_MODE_MUTEX);
  mtev_hash_init_mtev_memory(&data->service_data, 200, MTEV_HASH_LOCK_MODE_MUTEX);
  data->path_squash_cardinality_factor = 200;
  /* unless overridden, we flush telemetry every 60 seconds. */
  data->metric_flush_frequency_ms = 60000;
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
metric_to_json(mtev_dyn_buffer_t *json, metric_key_t *k, metric_value_t *v, bool resolve_avg, bool reset, bool jitter)
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
      /* inbound timestamps are centered within the minute
       * 
       * for every flush we do, add a random number between 0 and 29999 to reduce the chance of overwrites
       * for any re-puts at the same timestamp.
       */
      uint64_t ts = k->timestamp;
      if (jitter) {
        ts += (rand() % 30000);
      }
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu", ts);
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
      uint64_t ts = k->timestamp;
      if (jitter) {
        ts += (rand() % 30000);
      }
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu", ts);
    }
    break;
  case METRIC_VALUE_TYPE_INTEGER:
    {
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_type\":\"L\",", 12);
      mtev_dyn_buffer_add_printf(json, "\"_value\": %" PRIu64 ",", v->metric.integer);
      mtev_dyn_buffer_add_printf(json, "\"_owner\": %u,", v->owner_id);
      uint64_t ts = k->timestamp;
      if (jitter) {
        ts += (rand() % 30000);
      }
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu", ts);
      if (reset) {
        ck_pr_store_64(&v->metric.integer, 0);
      }
    }
    break;
  case METRIC_VALUE_TYPE_HISTOGRAM:
    {
      mtev_dyn_buffer_add(json, (uint8_t *)"\"_type\":\"h\",", 12);
      uint64_t ts = k->timestamp;
      if (jitter) {
        ts += (rand() % 30000);
      }
      mtev_dyn_buffer_add_printf(json, "\"_ts\": %llu,", ts);
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
  mtevL(mtev_debug, "Publishing off, not sending to kafka: %.*s\n", (int)mtev_dyn_buffer_used(json), mtev_dyn_buffer_data(json));
  return true;
#else
  return tm_kafka_produce(agg_topic, mtev_dyn_buffer_data(json), mtev_dyn_buffer_used(json), key, key_len);
#endif
}

static bool
send_regex_to_kafka(const char *service_name, const char *regex, const char *replace, tm_kafka_topic_t *regex_topic)
{
  if (regex == NULL || *regex == '\0') return true;

  /* this should remain entirely on the stack */
  mtev_dyn_buffer_t kafka;
  mtev_dyn_buffer_init(&kafka);
  mtev_dyn_buffer_ensure(&kafka, 1024);
  mtev_dyn_buffer_add(&kafka, (uint8_t *)"{", 1);
  /* fake the APM fields */
  mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{\"%s\": \"%s\"},", "processor", "event", "regex");
  mtev_dyn_buffer_add_printf(&kafka, "\"context\":{\"service\":{\"name\": \"%s\"}},", service_name);
  mtev_dyn_buffer_add_printf(&kafka, "\"regex\":\"%s\",", regex);
  mtev_dyn_buffer_add_printf(&kafka, "\"replace\":\"%s\"", replace);
  mtev_dyn_buffer_add(&kafka, (uint8_t *)"}", 1);

  bool rval = false;
/* #ifdef NO_PUBLISH */
/*   mtevL(mtev_error, "Publishing off, not sending url to kafka: %s\n", url); */
/*   rval = true; */
/* #else */
  /* we publish using the service name and regex as the key */
  char key[1024];
  size_t key_len = snprintf(key, sizeof(key), "%s|%s", service_name, regex);
  rval = tm_kafka_produce(regex_topic, mtev_dyn_buffer_data(&kafka), mtev_dyn_buffer_used(&kafka), 
                          key, key_len);
/* #endif */
  mtev_dyn_buffer_destroy(&kafka);
  return rval;
}


void tm_flush_team_metrics(mtev_hash_table *teams, tm_kafka_topic_t *agg_topic, tm_kafka_topic_t *regex_topic)
{
  //char sum_key[4096];
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

      bool debug_output = strstr(k->metric_name, "bdata-playbuilder-beast-test-qa") != NULL;
      /* delete when the flushed_ms >= last_seen.
       * 
       * The idea here is that we do an immediate flush but straggler data might come in 
       * and make the numbers more accurate over time, so we reflush with the updated numbers
       * but if nothing new came in for a while (10 minutes) we can delete it 
       */
      if (v->flushed_ms > 0 && v->flushed_ms >= v->last_seen && now > v->last_seen && now - v->last_seen > 600000) {
        if (debug_output) {
          mtevL(mtev_error, 
                "METRIC_TS: %" PRIu64 ", flushed_ms: %" PRIu64 ", last_seen: %" PRIu64 ", now: %" PRIu64 ", deleting\n", 
                k->timestamp, v->flushed_ms, v->last_seen, now);
        }
        v->scheduled_for_destruction = true;
        kafka_expired_list[kafka_expired_count] = k;
        kafka_expired_count++;
        continue;
      }

      /* otherwise, if we have already flushed this but it hasn't been 10 minutes, move along */
      if (v->flushed_ms > 0 && v->flushed_ms >= v->last_seen) {
        if (debug_output) {
          mtevL(mtev_error, 
                "METRIC_TS: %" PRIu64 ", flushed_ms: %" PRIu64 ", last_seen: %" PRIu64 ", now: %" PRIu64 ", skipping\n", 
                k->timestamp, v->flushed_ms, v->last_seen, now);
        }
        continue;
      }


      if (k->aggregate == false) {
        if (k->immediate_flush || ((now > k->timestamp && now - k->timestamp > 1000) && (now > v->last_seen && now - v->last_seen > 10000))) {
          if (debug_output) {
            mtevL(mtev_error, 
                  "METRIC_TS: %" PRIu64 ", flushed_ms: %" PRIu64 ", last_seen: %" PRIu64 ", now: %" PRIu64 ", sending metric value: %" PRIu64 "\n", 
                  k->timestamp, v->flushed_ms, v->last_seen, now, v->metric.integer);
          }

          if (comma) {
            mtev_dyn_buffer_add(&json, (uint8_t *)",", 1);
          }
          comma = true;

          /* floor to a 5 minute window */
          /* uint64_t window = k->timestamp - (k->timestamp % 300000); */
          /* mtev_hyperloglog_t *hll; */
          /* if (!mtev_hash_retrieve(&td->hlls, (const char *)&window, sizeof(window), (void **)&hll)) { */
          /*   hll = mtev_hyperloglog_alloc(14); */
          /*   uint64_t *s = malloc(sizeof(uint64_t)); */
          /*   *s = window; */
          /*   mtev_hash_store(&td->hlls, (const char *)s, sizeof(window), (void *)hll); */
          /* } */

          /* place the keys that we need to flush in a list for later deletion */
          json_expired_list[json_expired_count] = v;
          json_expired_count++;

          //mtev_hyperloglog_add(hll, k->metric_name, strlen(k->metric_name));
          metric_to_json(&json, k, v, true, false, true);
          /* if (v->type == METRIC_VALUE_TYPE_NUMBER) { */
          /*   /\* also write the SUM under a new key *\/ */

          /*   metric_key_t *sum_key = calloc(1, sizeof(metric_key_t) + k->key_len + 7); */
          /*   memcpy(sum_key->metric_name, k->metric_name, k->key_len) */

          /* } */
        }
      }
      else {
        /* for aggregates, send to kafka topic after a very short delay, they will be agged on the other side */
        if ((now > v->last_seen && now - v->last_seen > 10000)) {
          if (debug_output) {
            mtevL(mtev_error, 
                  "METRIC_TS: %" PRIu64 ", flushed_ms: %" PRIu64 ", last_seen: %" PRIu64 ", now: %" PRIu64 ", sending agg topic: %" PRIu64 "\n", 
                  k->timestamp, v->flushed_ms, v->last_seen, now, v->metric.integer);
          }

          mtev_dyn_buffer_reset(&kafka);
          mtev_dyn_buffer_add(&kafka, (uint8_t *)"{", 1);
          /* fake the APM fields */
          mtev_dyn_buffer_ensure(&kafka, 1024);
          mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{\"%s\": \"%s\"},", "processor", "event", "aggregate");
          mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{},", "context");

          /* build json */
          metric_to_json(&kafka, k, v, false, true, false);
          reset_value(v);

          mtev_dyn_buffer_add(&kafka, (uint8_t *)"}", 1);
          if (send_aggregate_to_kafka(&kafka, k->metric_name, strlen(k->metric_name), td, agg_topic)) {
            v->flushed_ms = now;
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

    /* 
     * to aid in accounting for metric utilization, emit a metric to the aggregation layer
     * that tracks the cardinality of time series seen for this team
     */
    /* uint64_t delete_windows[20] = {0}; */
    /* int delete_count = 0; */
    /* mtev_hash_iter hllit = MTEV_HASH_ITER_ZERO; */
    /* while(mtev_hash_adv(&td->hlls, &hllit)) { */
    /*   uint64_t window = *(uint64_t*)hllit.key.ptr; */
    /*   if (now > window && now - window >= 900000) { */
    /*     if (delete_count < 19) { */
    /*       delete_windows[delete_count] = window; */
    /*       delete_count++; */
    /*     } */

    /*     mtev_dyn_buffer_reset(&kafka); */
    /*     mtev_dyn_buffer_add(&kafka, (uint8_t *)"{", 1); */
    /*     /\* fake the APM fields *\/ */
    /*     mtev_dyn_buffer_ensure(&kafka, 1024); */
    /*     mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{\"%s\": \"%s\"},", "processor", "event", "aggregate"); */
    /*     mtev_dyn_buffer_add_printf(&kafka, "\"%s\":{},", "context"); */

    /*     char card[256] = {0}; */
    /*     sprintf(card, "time_series_cardinality|ST[team:%s,service:%s-tracemate]", name, name); */
    /*     metric_key_t *card_key = make_metric_key(card, window); */
    /*     card_key->aggregate = true; */
    /*     metric_value_t *card_value = make_integer_value(mtev_hyperloglog_size((mtev_hyperloglog_t*)hllit.value.ptr)); */

    /*     /\* build json *\/ */
    /*     metric_to_json(&kafka, card_key, card_value, false, true, false); */

    /*     mtev_dyn_buffer_add(&kafka, (uint8_t *)"}", 1); */
    /*     send_aggregate_to_kafka(&kafka, card_key->metric_name, strlen(card_key->metric_name), td, agg_topic); */
    /*     metric_key_free(card_key); */
    /*     metric_value_free(card_value); */
    /*   } */
    /* } */
    /* for (int i = 0; i < delete_count; i++) { */
    /*   mtev_hash_delete(&td->hlls, (const char *)&delete_windows[i], sizeof(uint64_t), free, (NoitHashFreeFunc)mtev_hyperloglog_destroy); */
    /* } */

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

    mtev_hash_iter ps = MTEV_HASH_ITER_ZERO;
    while(mtev_hash_adv(&td->path_squashers, &ps)) {
      mtev_hash_table *regexes = tm_path_squasher_get_regexes((tm_path_squasher_t *)ps.value.ptr);
      mtev_hash_iter regex = MTEV_HASH_ITER_ZERO;
      while(mtev_hash_adv(regexes, &regex)) {
        pcre_matcher *m = (pcre_matcher *)regex.value.ptr;

        /* cross reference this regex with the stuff we already know about and don't publish
         * if it exists in the td->squash_regexes table.  The td->squash_regexes table
         * is only filled by reading from the kafka 'tracemate_regexes' topic which is the 
         * source of truth.  So publishing again would just create duplicates for this service
         */
        mtev_hash_table *x = NULL;
        if (mtev_hash_retrieve(&td->squash_regexes, ps.key.str, strlen(ps.key.str) + 1, (void **)&x)) {
          if (!mtev_hash_retrieve(x, regex.key.str, strlen(regex.key.str) + 1, NULL)) {
            /* the regex.key is the regex string itself, the ps.key is the service_name 
             * 
             * We are going to publish a JSON document which contains the regex and
             * replace string as the value and the key is the service_name and regex
             * as a concatenated striing.
             * 
             * The topic this is published to uses a compact strategy effectively overwriting
             * the same regex over and over again.
             */
            send_regex_to_kafka(ps.key.str, regex.key.str, m->replace, regex_topic);
          }
        }
      }
      mtev_hash_destroy(regexes, free, pcre_matcher_destroy);
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
