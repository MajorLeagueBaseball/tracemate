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

#define RETURN_IF_MISSING(result,o,field)                               \
  mtev_json_object *result = NULL;                                      \
  do {                                                                  \
    if (o == NULL) return false;                                        \
    result = mtev_json_object_object_get(o, field);                     \
    if (result == NULL) {                                               \
      mtevL(tm_error, "'span' event is malformed, no '%s' object\n", field); \
      return false;                                                     \
    }                                                                   \
  } while(false)

#if 0
static bool
parse_stmt(const char *stmt, char *table, size_t table_len)
{
  int erroff, ovector[30];
  const char *pcre_err;
  char **err = NULL;

  if(!sql_match) {
    sql_match = pcre_compile("^.*\\s+from\\s+([^\\s;]*).*$",
                             PCRE_CASELESS, &pcre_err, &erroff, NULL);
    if(!sql_match) {
      *err = "sql match pattern failed to compile!";
      mtevL(tm_error, "pcre_compiled failed offset %d: %s\n", erroff, pcre_err);
      return false;
    }
  }
  if (stmt == NULL) return false;
  size_t len = strlen(stmt);
  int rc = 0;
  if ((rc = pcre_exec(sql_match, NULL, stmt, len, 0, 0, ovector, 30)) == 2) {
    const char *t;
    pcre_get_substring(stmt, ovector, rc, 1, &t);
    strncpy(table, t, table_len - 1);
    /* strip out double quotes if there are any */
    for (int i = 0; i < strlen(table); i++) {
      if (table[i] == '"') {
        table[i] = '\'';
      }
    }
    pcre_free_substring(t);
    return true;
  }
  return false;
}
#endif

static char*
parse_v6_type(const char *type)
{
  // an elastic apm 6.X span "type" resembles:
  //
  // db.postgresql.query
  // db.redis.query
  // external.http.get

  // we care about the 2nd segment here.

  int idx = 0;
  char *subtype = calloc(1, 20);
  bool in_segment = false;
  for (int i = 0; i < strlen(type); i++) {
    if (type[i] == '.' && in_segment == false) {
      in_segment = true;
      continue;
    }
    if (type[i] == '.' && in_segment) {
      return subtype;
    }
    if (in_segment) {
      subtype[idx++] = type[i];
    }
  }
  if (in_segment == false) {
    //we never hit a dot, use "unknown"
    memcpy(subtype, "unknown", 7);
  }
  return subtype;
}

bool process_span_message(topic_stats_t *stats, mtev_json_object *message, int ttl)
{
  char metric_name[4096];
  char tag_string[3172];
  char agg_tag_string[2048];
  char team[128];
  uint64_t timestamp;
  mtev_hash_table *team_metrics = pre_process(message, team, tag_string, &timestamp);
  team_data_t *td = get_team_data(team);

  if (team_metrics == NULL || td == NULL) return false;

  mtev_json_object *span = mtev_json_object_object_get(message, "span");
  if (span == NULL) {
    mtevL(tm_error, "'span' event is malformed, no 'span' object\n");
    return false;
  }

  if (!build_agg_tag_string(message, team, agg_tag_string)) {
    return false;
  }

  mtev_json_object *dur = mtev_json_object_object_get(span, "duration");
  if (dur == NULL) {
    return false;
  }

  uint64_t duration_us = mtev_json_object_get_uint64(mtev_json_object_object_get(dur, "us"));

  /*
   *
   *
   "span": {
   "name": "SELECT",
   "type": "db.db2.query",
   "duration": {
   "us": 6981
   },
   *
   *
   *  "context": {
   "db": {
   "statement": "SELECT 1 FROM SYSIBM.SYSDUMMY1",
   "type": "sql",
   "user": "ebisapp"
   },
   "service": {
   "agent": {
   "name": "java",
   "version": "1.4.0"
   },
   "name": "best-ebis-amt"
   }
   },
   *                                                vv first 256 chars of stmt  vv elipsis if it's longer
   * Make a metric like: "db.db2.query - SELECT - (SELECT 1 FROM SYSIBM.SYSDUMMY1...) - latency|ST[tags]"
   *                      {span.type}    {span.name}  {context.db.statement}
   *
   * Make an aggregate metric like: "db.db2.query - SELECT - latency|ST[tags]"
   *                      {span.type}    {span.name}  {context.db.statement}
  */

  mtev_json_object *trans = mtev_json_object_object_get(message, "transaction");
  if (trans == NULL) {
    mtevL(tm_error, "'span' event is malformed, no 'transaction' object\n");
    return false;
  }

  mtev_json_object *trace_json = mtev_json_object_object_get(message, "trace");
  if (trace_json == NULL) {
    mtevL(tm_error, "'span' event is malformed, no 'trace' object\n");
    return false;
  }
  mtev_json_object *trace_id_json = mtev_json_object_object_get(trace_json, "id");
  const char *trace_id = mtev_json_object_get_string(trace_id_json);
  if (trace_id == NULL) return false;

  /* save this span for jaegerizing (maybe) */
  tm_transaction_store_add_child(trace_id, strlen(trace_id), message, ttl);

  mtev_json_object *type = mtev_json_object_object_get(span, "type");

  const char *service_string = tm_service_name(message);

  const char *t = mtev_json_object_get_string(type);

  /* certain high cardinality metrics can contain a special tag that prevents them from being rolled up
   * for longer term storage
   */
  const char *rollup = "";
  if (td->rollup_high_cardinality == mtev_false) {
    rollup = ",__rollup:false";
  }

  int mv = tm_apm_server_major_version(message);

  if (strncmp(t, "db", 2) == 0) {
    const char *operation = mtev_json_object_get_string(mtev_json_object_object_get(span, "name"));
    uint64_t agg_timestamp = ceil_timestamp(timestamp);

    /* aggregate for statement */
    snprintf(metric_name, sizeof(metric_name) - 1, "db - latency - %s|ST[%s%s]",
             operation, agg_tag_string, rollup);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* host and statement specific */
    if (td->collect_host_level_metrics) {
      snprintf(metric_name, sizeof(metric_name) - 1, "db - latency - %s|ST[%s%s]",
               operation, tag_string, rollup);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);
      snprintf(metric_name, sizeof(metric_name) - 1, "db - stmt_count - %s|ST[%s%s]",
               operation, tag_string, rollup);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
    }

    const char *subtype = NULL;
    bool free_subtype = false;
    if (mv > 6) {
      subtype = mtev_json_object_get_string(mtev_json_object_object_get(span, "subtype"));
    } else {
      subtype = parse_v6_type(t);
      free_subtype = true;
    }

    if (subtype != NULL) {
      /* aggregate for subtype operation */
      snprintf(metric_name, sizeof(metric_name) - 1, "db - latency - %s - %s|ST[%s%s]",
               subtype, operation, agg_tag_string, rollup);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "db - stmt_count - %s - %s|ST[%s%s]",
               subtype, operation, tag_string, rollup);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "db - latency - %s|ST[%s%s]",
               subtype, agg_tag_string, rollup);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "db - stmt_count - %s|ST[%s]",
               subtype, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
      if (free_subtype) {
        free((char *)subtype);
      }
    }

    /* aggregate for the service */
    snprintf(metric_name, sizeof(metric_name) - 1, "db - latency - all|ST[%s]",
             agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "db - stmt_count - all|ST[%s]",
             agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

  }
  else if (strcmp(t, "external.http") == 0 || strcmp(t, "ext.http.http") == 0 || strcmp(t, "external") == 0) {

    const char *ex_name = mtev_json_object_get_string(mtev_json_object_object_get(span, "name"));

    /* the name might contain a URL, e.g.:
     *
     *  GET https://search-api.mlb.com/svc/search/v2/mlb_global_en/topic/69972428
     *
     * We need to genericize this
     */
    char *external_name = genericize_path(service_string, ex_name, td);

    uint64_t agg_timestamp = ceil_timestamp(timestamp);

    /* aggregate for external */
    snprintf(metric_name, sizeof(metric_name) - 1, "external - latency - %s|ST[%s%s]",
             external_name, agg_tag_string, rollup);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* aggregate for service */
    snprintf(metric_name, sizeof(metric_name) - 1, "external - latency - all|ST[%s]",
             agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    if (td->collect_host_level_metrics) {
      /* host and statement specific */
      snprintf(metric_name, sizeof(metric_name) - 1, "external - latency - %s|ST[%s%s]",
               external_name, tag_string, rollup);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      /* counts */
      snprintf(metric_name, sizeof(metric_name) - 1, "external - call_count - %s|ST[%s%s]",
               external_name, tag_string, rollup);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
    }

    snprintf(metric_name, sizeof(metric_name) - 1, "external - call_count - all|ST[%s]",
             agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    free(external_name);
  }
  else {
    mtevL(tm_debug, "Unprocessed span type: %s\n", mtev_json_object_get_string(type));
  }
  return false;
}

#undef RETURN_IF_MISSING
