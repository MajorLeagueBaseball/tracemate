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

#ifndef TM_UTILS_H
#define TM_UTILS_H

#include <circllhist.h>
#include <circmetrics.h>
#include <eventer/eventer.h>

#include "tm_log.h"

#include <stdio.h>

#include <pcre.h>

typedef struct tm_kafka_topic tm_kafka_topic_t;

typedef struct topic_stats {
  tm_kafka_topic_t *topic;
  eventer_jobq_t *kafka_read_jobq;
  eventer_jobq_t *kafka_process_jobq;
  stats_handle_t *messages_seen;
  stats_handle_t *messages_filtered;
  stats_handle_t *messages_processed;
  stats_handle_t *messages_errored;
  stats_handle_t *messages_orphaned;
  stats_handle_t *metrics_flushed;
  stats_handle_t *unknown_team;
  uint64_t kafka_offset;
  stats_handle_t *kafka_offset_rob;
  int64_t high_watermark;
  stats_handle_t *high_watermark_rob;
  int64_t kafka_lag;
  int32_t read_delay_ms;
  stats_handle_t *kafka_lag_rob;
  stats_handle_t *message_process_latency;
} topic_stats_t;

typedef struct team_data team_data_t;

static inline uint64_t floor_timestamp(const uint64_t ms, const uint64_t flush_freq_ms)
{
  return ms - (ms % flush_freq_ms);
}

static inline uint64_t center_timestamp(const uint64_t ms, const uint64_t flush_freq_ms)
{
  return floor_timestamp(ms, flush_freq_ms) + (flush_freq_ms / 2);
}

static inline uint64_t ceil_timestamp(const uint64_t ms, const uint64_t flush_freq_ms)
{
  return floor_timestamp(ms, flush_freq_ms) + (flush_freq_ms - 1); // stick it in the last ms of the frequency
}

static inline uint64_t parse_timestamp(const char *ts)
{
  /* 2019-03-20T17:09:03.949Z */

  int y,M,d,h,m,s,ms;
  sscanf(ts, "%d-%d-%dT%d:%d:%d.%dZ", &y, &M, &d, &h, &m, &s, &ms);
  struct tm time;
  time.tm_year = y - 1900; // Year since 1900
  time.tm_mon = M - 1;     // 0-11
  time.tm_mday = d;        // 1-31
  time.tm_hour = h;        // 0-23
  time.tm_min = m;         // 0-59
  time.tm_sec = s;         // 0-61

  time_t t = mktime(&time);
  return (t * 1000) + ms;
}

/* take an incoming URL, split by '/'.  If any
 * token is purely numeric, convert to '{id}'.
 * Otherwise, if it's a guid, convert to '{guid}'
 * 
 * Take the last path section and completely elide if it's a file (contains a dot)
 */
char *genericize_path(const char *service_name, const char *str, team_data_t *td);
bool is_path_ok(const char *str, team_data_t *td);

tm_kafka_topic_t *get_span_producer_topic();
tm_kafka_topic_t *get_error_producer_topic();
tm_kafka_topic_t *get_transaction_producer_topic();

uint64_t get_jaeger_threshold_us(const char *service_name);
uint64_t get_metric_flush_frequency_ms(const char *service_name);

void init_path_regex(tm_kafka_topic_t *url_topic);

char *get_oauth2_token(const char *private_key_id, const char *private_key,
                       const char *service_account, const char *scope, const char *aud,
                       uint64_t* expires_epoch_ms);

char *tm_replace_vars(const char *source, mtev_hash_table *values);

#endif
