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

#ifndef TM_PROCESS_H
#define TM_PROCESS_H

#include <stdint.h>
#include <mtev_hooks.h>
#include <mtev_json_object.h>

#include "tm_utils.h"
#include "tm_metric.h"
#include "tm_transaction_store.h"

static inline int tm_apm_server_major_version(mtev_json_object *m)
{
  char vv[8];

  if (m == NULL) return 0;

  mtev_json_object *md = mtev_json_object_object_get(m, "@metadata");
  if (md == NULL) return 6;

  mtev_json_object *version = mtev_json_object_object_get(md, "version");
  if (version == NULL) return 6;

  const char *v = mtev_json_object_get_string(version);
  if (strlen(v) < 1) return 6;

  memcpy(vv, v, sizeof(vv) - 1);
  vv[1] = '\0';

  return atoi(vv);
}

static inline char* tm_apm_server_version(mtev_json_object *m)
{
  if (m == NULL) return NULL;

  mtev_json_object *md = mtev_json_object_object_get(m, "@metadata");
  if (md == NULL) return NULL;

  mtev_json_object *version = mtev_json_object_object_get(md, "version");
  if (version == NULL) return NULL;

  const char *v = mtev_json_object_get_string(version);
  if (strlen(v) < 1) return NULL;

  return strdup(v);
}

static inline const char *tm_service_name(mtev_json_object *m)
{
  int mv = tm_apm_server_major_version(m);
  if (mv == 0) {
    // might be missing an @metadata stanza, try version 6
    mv = 6;
  }
  if (mv < 6) return NULL; //we can't process major versions earlier than 6.X

  mtev_json_object* root = m;
  if (mv == 6) {
    root = mtev_json_object_object_get(m, "context");
    if (root == NULL) return NULL;
  }
  mtev_json_object *service = mtev_json_object_object_get(root, "service");
  if (service == NULL) return NULL;
  mtev_json_object *name = mtev_json_object_object_get(service, "name");
  if (name == NULL) return NULL;
  return mtev_json_object_get_string(name);
}

static inline const char *tm_service_env(mtev_json_object *m)
{
  int mv = tm_apm_server_major_version(m);
  if (mv < 6) return NULL; //we can't process major versions earlier than 6.X

  mtev_json_object* root = m;
  if (mv == 6) {
    root = mtev_json_object_object_get(m, "context");
    if (root == NULL) return NULL;
  }

  mtev_json_object *service = mtev_json_object_object_get(root, "service");
  if (service == NULL) return NULL;
  mtev_json_object *name = mtev_json_object_object_get(service, "environment");
  if (name == NULL) return NULL;
  return mtev_json_object_get_string(name);
}


static inline const bool tm_get_team(const char *sn, char *dest)
{
  if (sn == NULL) return false;
  const char *first_dash = strchr(sn, '-');
  if (first_dash == NULL) return false;
  strncpy(dest, sn, first_dash - sn);
  dest[first_dash - sn] = '\0';
  return true;
}


size_t tm_clean_tag_value(const char *tag, char *dest);
mtev_hash_table* pre_process(mtev_json_object *message, char *team, char *tag_string, uint64_t *timestamp);

bool build_agg_tag_string(mtev_json_object *message, const char *team, char *tag_string);

/* these return true if they want to keep the json object they were passed */
bool process_metric_message(topic_stats_t *stats, mtev_json_object *message);
bool process_transaction_message(topic_stats_t *stats, mtev_json_object *message, int ttl);
bool process_aggregate_message(topic_stats_t *stats, mtev_json_object *message);
bool process_span_message(topic_stats_t *stats, mtev_json_object *message, int ttl);
bool process_error_message(topic_stats_t *stats, mtev_json_object *message, int ttl);
bool process_url_message(topic_stats_t *stats, mtev_json_object *message);
bool process_regex_message(topic_stats_t *stats, mtev_json_object *message);

bool process_error_message_with_root(mtev_json_object *message, tm_transaction_store_entry_t *root_span, int ttl);

void update_histogram(team_data_t *td,
                      mtev_hash_table *team_metrics,
                      const char *metric_name,
                      bool aggregate,
                      uint64_t duration_us,
                      uint64_t timestamp);

void update_counter(team_data_t *td,
                    mtev_hash_table *team_metrics,
                    const char *metric_name,
                    bool aggregate,
                    uint64_t count,
                    uint64_t timestamp);

void update_text(team_data_t *td,
                 mtev_hash_table *team_metrics,
                 const char *metric_name,
                 const char *text,
                 uint64_t timestamp);

void update_numeric(team_data_t *td,
                    mtev_hash_table *team_metrics,
                    const char *metric_name,
                    bool aggregate,
                    double value,
                    uint64_t timestamp);

void update_average(team_data_t *td,
                    mtev_hash_table *team_metrics,
                    const char *metric_name,
                    bool aggregate,
                    double value,
                    uint64_t timestamp);

void reset_value(metric_value_t *value);

#endif
