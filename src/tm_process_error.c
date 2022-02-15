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
#include "tm_jaeger.h"
#include "tm_metric.h"
#include "tm_transaction_store.h"
#include "tm_process.h"
#include "tm_utils.h"

bool process_error_message(topic_stats_t *stats, mtev_json_object *message, int ttl)
{
  /* error message is special, it causes the parent transaction to be traced immediately 
   * 
   */
  char tag_string[3172];
  char agg_tag_string[2048];
  char metric_name[4096];
  char team[128];
  uint64_t timestamp;
  mtev_hash_table *team_metrics = pre_process(message, team, tag_string, &timestamp);
  team_data_t *td = get_team_data(team);

  if (team_metrics == NULL || td == NULL) return false;

  if (!build_agg_tag_string(message, team, agg_tag_string)) {
    return false;
  }

  const char *service_string = tm_service_name(message);
  if (service_string == NULL) {
    mtevL(tm_error, "'error' event is malformed, service name missing\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  uint64_t metric_flush_freq_ms = get_metric_flush_frequency_ms(service_string);

  mtev_json_object *error = mtev_json_object_object_get(message, "error");
  if (error == NULL) {
    mtevL(tm_error, "'error' event is malformed, no 'error' object\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  mtev_json_object *trace_json = mtev_json_object_object_get(message, "trace");
  if (trace_json == NULL) {
    mtevL(tm_debug, "'error' event is malformed, no 'trace' object, probably error on unsampled transaction\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  mtev_json_object *trace_id_json = mtev_json_object_object_get(trace_json, "id");
  const char *trace_id = mtev_json_object_get_string(trace_id_json);

  tm_transaction_store_add_child(trace_id, strlen(trace_id), message, ttl);
  tm_transaction_store_mark_traceable(trace_id, strlen(trace_id));

  uint64_t agg_timestamp = center_timestamp(timestamp, metric_flush_freq_ms);

  /* certain high cardinality metrics can contain a special tag that prevents them from being rolled up
   * for longer term storage
   */
  const char *rollup = "";
  if (td->rollup_high_cardinality == mtev_false) {
    rollup = ",__rollup:false";
  }

  if (td->collect_host_level_metrics) {
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - apm_error_count|ST[%s%s]",
             tag_string, rollup);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
  }

  snprintf(metric_name, sizeof(metric_name) - 1, "transaction - apm_error_count|ST[%s]",
           agg_tag_string);
  update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

  return false;
}
