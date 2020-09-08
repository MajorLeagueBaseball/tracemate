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

#include "tm_process.h"

#include "tm_log.h"
#include "tm_metric.h"

/***** Below 2 maps borrowed from reconnoiter code ********/

/*
 * map for ascii tags
  perl -e '$valid = qr/[`+A-Za-z0-9!@#\$%^&"'\/\?\._-]/;
  foreach $i (0..7) {
  foreach $j (0..31) { printf "%d,", chr($i*32+$j) =~ $valid; }
  print "\n";
  }'
*/
/* static uint8_t vtagmap_key[256] = { */
/*   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, */
/*   0,1,1,1,1,1,1,1,0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,1, */
/*   1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1, */
/*   1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0, */
/*   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, */
/*   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, */
/*   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, */
/*   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 */
/* }; */

/* Same as above, but allow for ':' and '=' */
static uint8_t vtagmap_value[256] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,1,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,0,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

size_t 
tm_clean_tag_value(const char *tag, char *dest)
{
  size_t l = strlen(tag);
  for (size_t i = 0; i < l; i++) {
    if (vtagmap_value[(uint8_t)tag[i]] == 0) {
      dest[i] = '_';
    } else {
      dest[i] = tag[i];
    }
  }
  dest[l] = '\0';
  return l;
}


mtev_hash_table* 
pre_process(mtev_json_object *message, char *team, char *tag_string, uint64_t *timestamp)
{
  char sn[256];
  const char *service_name = tm_service_name(message);
  if (!tm_get_team(service_name, team)) {
    mtevL(tm_error, "Failed to get team name from: %s\n", service_name);
    return NULL;
  }
  strcpy(sn, service_name + strlen(team) + 1);

  mtev_hash_table *team_metrics = get_team_metrics_table(team);
  if (!team_metrics) {
    mtevL(tm_debug, "Team not configured: %s\n", team);
    return NULL;
  }

   /* Where context.tags.name indicates the GC phase
   *
   *
   * The "context" object will have a "system" child which contains tag info
   *
   "context": {
     "system": {
       "hostname": "abfc88eec189",
       "architecture": "amd64",
       "platform": "Linux",
       "ip": "10.75.8.21"
     },
     ...
   }
  */

  /* build the name and tags into the metric_key_t */
  const char *ts = mtev_json_object_get_string(mtev_json_object_object_get(message, "@timestamp"));
  *timestamp = parse_timestamp(ts);

  mtev_json_object *context = mtev_json_object_object_get(message, "context");

  /* build our tag string first as this will be shared for each metric */
  char *tags = tag_string;
  tags += sprintf(tags, "service:%s", service_name);
  if (context) {
    mtev_json_object *sys = mtev_json_object_object_get(context, "system");
    if (sys) {
      mtev_json_object *host = mtev_json_object_object_get(sys, "hostname");
      mtev_json_object *ip = mtev_json_object_object_get(sys, "ip");
      if (host) {
        tags += sprintf(tags, ",host:");
        tags += tm_clean_tag_value(mtev_json_object_get_string(host), tags);
      }
      if (ip) {
        tags += sprintf(tags, ",ip:");
        tags += tm_clean_tag_value(mtev_json_object_get_string(ip), tags);
      }
    }
  }
  return team_metrics;
}

bool
build_agg_tag_string(mtev_json_object *message, const char *team, char *tag_string)
{
  const char *service_name = tm_service_name(message);
  mtev_hash_table *team_metrics = get_team_metrics_table(team);
  if (!team_metrics) {
    mtevL(tm_debug, "Team not configured: %s\n", team);
    return false;
  }
  char *tags = tag_string;
  tags += sprintf(tags, "service:%s,host:all,ip:all", service_name);
  return true;
}


void
update_histogram(team_data_t *td, mtev_hash_table *team_metrics, const char *metric_name, bool aggregate, uint64_t duration_us, uint64_t timestamp)
{
  uint64_t now = mtev_now_ms();

  if (now > timestamp && now - timestamp > 600000) {
    mtevL(mtev_debug, "Got update for >= 10 minute old metric, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
  }
  metric_key_t *key = make_metric_key(metric_name, timestamp);
  key->aggregate = aggregate;
  void *existing = NULL;
  if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
    metric_value_t *v = (metric_value_t *)existing;
    if (v->scheduled_for_destruction) {
      mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
    }
    ck_pr_store_64(&v->last_seen, mtev_now_ms());
    ck_spinlock_lock(&v->histogram_lock);
    hist_insert_intscale(v->metric.hist, duration_us, -6, 1);
    ck_spinlock_unlock(&v->histogram_lock);
    metric_key_free(key);
  } else {
    pthread_mutex_lock(&td->mutex);
    if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
      metric_value_t *v = (metric_value_t *)existing;
      if (v->scheduled_for_destruction) {
        mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
      }
      ck_pr_store_64(&v->last_seen, mtev_now_ms());
      ck_spinlock_lock(&v->histogram_lock);
      hist_insert_intscale(v->metric.hist, duration_us, -6, 1);
      ck_spinlock_unlock(&v->histogram_lock);
      metric_key_free(key);
    } else {
      metric_value_t *v = make_histogram_latency_value(duration_us);
      mtev_hash_store(team_metrics, (const char *)key, key->key_len, v);
    }
    pthread_mutex_unlock(&td->mutex);
  }
}

void 
update_counter(team_data_t *td, mtev_hash_table *team_metrics, const char *metric_name, bool aggregate, uint64_t count, uint64_t timestamp)
{
  uint64_t now = mtev_now_ms();
  metric_key_t *key = make_metric_key(metric_name, timestamp);
  key->aggregate = aggregate;
  key->immediate_flush = !aggregate;
  void *existing = NULL;
  if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
    metric_value_t *v = (metric_value_t *)existing;
    if (v->scheduled_for_destruction) {
      mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
    }
    ck_pr_store_64(&v->last_seen, mtev_now_ms());
    ck_pr_add_64(&v->metric.integer, count);
    metric_key_free(key);
  } else {
    pthread_mutex_lock(&td->mutex);
    if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
      metric_value_t *v = (metric_value_t *)existing;
      if (v->scheduled_for_destruction) {
        mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
      }
      ck_pr_store_64(&v->last_seen, mtev_now_ms());
      ck_pr_add_64(&v->metric.integer, count);
      metric_key_free(key);
    } else {
      metric_value_t *v = make_integer_value(count);
      mtev_hash_store(team_metrics, (const char *)key, key->key_len, v);
    }
    pthread_mutex_unlock(&td->mutex);
  }
}

void 
update_text(team_data_t *td, mtev_hash_table *team_metrics, const char *metric_name, const char *text, uint64_t timestamp)
{
  metric_key_t *key = make_metric_key(metric_name, timestamp);
  key->aggregate = false;
  key->immediate_flush = true;
  mtev_hash_replace(team_metrics, (const char *)key, key->key_len, make_text_value(text), metric_key_free, metric_value_free);
}

void 
update_numeric(team_data_t *td, mtev_hash_table *team_metrics, const char *metric_name, bool aggregate, double value, uint64_t timestamp)
{
  uint64_t now = mtev_now_ms();
  metric_key_t *key = make_metric_key(metric_name, timestamp);
  key->aggregate = aggregate;
  key->immediate_flush  = !aggregate;
  void *existing = NULL;
  if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
    metric_value_t *v = (metric_value_t *)existing;
    ck_pr_store_64(&v->last_seen, mtev_now_ms());
    if (v->scheduled_for_destruction) {
      mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
    }
    ck_pr_add_double(&v->metric.number, value);
    metric_key_free(key);
  } else {
    pthread_mutex_lock(&td->mutex);
    if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
      metric_value_t *v = (metric_value_t *)existing;
      ck_pr_store_64(&v->last_seen, mtev_now_ms());
      if (v->scheduled_for_destruction) {
        mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
      }
      ck_pr_add_double(&v->metric.number, value);
      metric_key_free(key);
    } else {
      metric_value_t *v = make_numeric_value(value);
      mtev_hash_store(team_metrics, (const char *)key, key->key_len, v);
    }
    pthread_mutex_unlock(&td->mutex);
  }
}

void 
update_average(team_data_t *td, mtev_hash_table *team_metrics, const char *metric_name, bool aggregate, double value, uint64_t timestamp)
{
  uint64_t now = mtev_now_ms();
  metric_key_t *key = make_metric_key(metric_name, timestamp);
  key->aggregate = aggregate;
  key->immediate_flush  = !aggregate;
  void *existing = NULL;
  if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
    metric_value_t *v = (metric_value_t *)existing;
    ck_pr_store_64(&v->last_seen, mtev_now_ms());
    if (v->scheduled_for_destruction) {
      mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
    }
    ck_pr_add_double(&v->metric.number, value);
    ck_pr_add_int(&v->count, 1);
    metric_key_free(key);
  } else {
    pthread_mutex_lock(&td->mutex);
    if (mtev_hash_retrieve(team_metrics, (const char *)key, key->key_len, &existing)) {
      metric_value_t *v = (metric_value_t *)existing;
      ck_pr_store_64(&v->last_seen, mtev_now_ms());
      if (v->scheduled_for_destruction) {
        mtevL(mtev_error, "Got update for metric scheduled for destruction, now: %" PRIu64 ", ts: %" PRIu64 ", name: %s, aggregate: %d\n", now, timestamp, metric_name, aggregate);
      }
      ck_pr_add_double(&v->metric.number, value);
      ck_pr_add_int(&v->count, 1);
      metric_key_free(key);
    } else {
      metric_value_t *v = make_numeric_value(value);
      mtev_hash_store(team_metrics, (const char *)key, key->key_len, v);
    }
    pthread_mutex_unlock(&td->mutex);
  }
}

void
reset_value(metric_value_t *v)
{
  v->count = 0;
  v->flushed_ms = 0;
  switch(v->type) {
  case METRIC_VALUE_TYPE_NUMBER:
    ck_pr_store_double(&v->metric.number, 0);
    break;
  case METRIC_VALUE_TYPE_TEXT:
    break;
  case METRIC_VALUE_TYPE_HISTOGRAM:
    ck_spinlock_lock(&v->histogram_lock);
    hist_free(v->metric.hist);
    v->metric.hist = hist_alloc();
    ck_spinlock_unlock(&v->histogram_lock);
    break;
  case METRIC_VALUE_TYPE_INTEGER:
    ck_pr_store_64(&v->metric.integer, 0);
    break;
  }
}
