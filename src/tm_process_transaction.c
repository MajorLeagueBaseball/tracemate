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

#include "tm_hooks.h"
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
      mtevL(tm_error, "'transaction' event is malformed, no '%s' object\n", field); \
      stats_add64(stats->messages_errored, 1);                          \
      return false;                                                     \
    }                                                                   \
  } while(false)

bool process_transaction_message(topic_stats_t *stats, mtev_json_object *message, int ttl)
{
  char metric_name[4096];
  char tag_string[3172];
  char agg_tag_string[2048];
  char team[128];
  char sc[32];
  uint64_t timestamp;
  mtev_hash_table *team_metrics = pre_process(message, team, tag_string, &timestamp);
  team_data_t *td = get_team_data(team);

  if (!build_agg_tag_string(message, team, agg_tag_string)) {
    return false;
  }

  if (team_metrics == NULL || td == NULL) return false;

  mtev_json_object *trans = mtev_json_object_object_get(message, "transaction");
  if (trans == NULL) {
    mtevL(tm_error, "'transaction' event is malformed, no 'transaction' object\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  mtev_json_object *duration = mtev_json_object_object_get(trans, "duration");
  if (duration == NULL) {
    mtevL(tm_error, "'transaction' event is malformed, no 'duration' object\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  uint64_t duration_us = mtev_json_object_get_uint64(mtev_json_object_object_get(duration, "us"));
  RETURN_IF_MISSING(trace_json, message, "trace");
  RETURN_IF_MISSING(trace_id_json, trace_json, "id");
  const char *trace_id = mtev_json_object_get_string(trace_id_json);

  RETURN_IF_MISSING(context, message, "context");
  RETURN_IF_MISSING(service, context, "service");
  RETURN_IF_MISSING(service_name, service, "name");
  char *clean_path = NULL;

  mtev_json_object *processed = mtev_json_object_object_get(message, "tracemate_processed");
  if (processed != NULL) {
    if (mtev_json_object_get_boolean(processed)) {
      goto process_jaeger;
    }
  }

  /* certain high cardinality metrics can contain a special tag that prevents them from being rolled up
   * for longer term storage
   */
  const char *rollup = "";
  if (td->rollup_high_cardinality == mtev_false) {
    rollup = ",__rollup:false";
  }

  //stackdriver_transaction(message, "cloudtrace.googleapis.com:443");

  /* process this transaction for histogramming, we *always* histogram at the parent transaction level,
   */
  const char *type = mtev_json_object_get_string(mtev_json_object_object_get(trans, "type"));

  if (strcmp(type, "message_read") == 0 || strcmp(type, "messaging") == 0) {
    /* build aggregate metrics, this replaces host specific info with `all` */
    uint64_t agg_timestamp = ceil_timestamp(timestamp);
    const char *name = mtev_json_object_get_string(mtev_json_object_object_get(trans, "name"));

    /* average latency for this service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s]",
             name, agg_tag_string);

    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* average latency for the service */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - all|ST[%s]",
             agg_tag_string);

    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* request count for the service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - %s|ST[%s]",
             name, agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - all|ST[%s]",
             agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    /* reuse metric_name for the latency metric name of this metric is the method and the clean_path */
    if (td->collect_host_level_metrics) {
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s]",
               name, tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);
    }
  }
  else if (strcmp(type, "scheduled") == 0) {
    /* build aggregate metrics, this replaces host specific info with `all` */
    uint64_t agg_timestamp = ceil_timestamp(timestamp);
    const char *name = mtev_json_object_get_string(mtev_json_object_object_get(trans, "name"));

    /* average latency for this service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s]",
             name, agg_tag_string);

    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* average latency for the service */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - all|ST[%s]",
             agg_tag_string);

    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* request count for the service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - %s|ST[%s]",
             name, agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - all|ST[%s]",
             agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    /* reuse metric_name for the latency metric name of this metric is the method and the clean_path */
    if (td->collect_host_level_metrics) {
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s]",
               name, tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);
    }
  }
  else if (strcmp(type, "request") == 0) {
    /*
     * request means that there is a context.request object
     * We can get the response code from the context.response object as well as the path and synthesize a metric out of it
     *
     * we can't rely on the transaction.name, these often have useless things like
     * "OPTIONS" or "GET" or "*" as the name.
     *
     * Instead, build the name based on the context.request.method, and the context.url.pathname
     */

    mtev_json_object *req = mtev_json_object_object_get(context, "request");
    mtev_json_object *resp = mtev_json_object_object_get(context, "response");
    if (req == NULL || resp == NULL) {
      mtevL(tm_error, "Transaction missing request or response object\n");
      stats_add64(stats->messages_errored, 1);
      return false;
    }
    mtev_json_object *status_code = mtev_json_object_object_get(resp, "status_code");
    mtev_json_object *method = mtev_json_object_object_get(req, "method");
    mtev_json_object *url = mtev_json_object_object_get(req, "url");
    mtev_json_object *path = mtev_json_object_object_get(url, "pathname");

    const char *meth = mtev_json_object_get_string(method);
    if (strcmp(meth, "OPTIONS") == 0) {
      stats_add64(stats->messages_filtered, 1);
      return false;
    }

    if (!is_path_ok(mtev_json_object_get_string(path), td)) {
      stats_add64(stats->messages_filtered, 1);
      return false;
    }

    int stat_code = mtev_json_object_get_int(status_code);
    if (stat_code == 404) {

      /* Don't bother tracking stats on URLs that don't exist */
      stats_add64(stats->messages_filtered, 1);
      return false;
    }

    /* remove any GUIDS or integers from the path to genericize it */
    clean_path = genericize_path(mtev_json_object_get_string(path), td);

    /* build aggregate metrics, this replaces host specific info with `all` */
    uint64_t agg_timestamp = ceil_timestamp(timestamp);

    /* average latency for this service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s,method:%s%s]",
             clean_path, agg_tag_string, mtev_json_object_get_string(method), rollup);

    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* average latency for the service */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - all|ST[%s,method:%s]",
             agg_tag_string, mtev_json_object_get_string(method));

    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    if (stat_code >= 400 && stat_code < 500) {
      /* client error count for the service and URL */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - client_error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, mtev_json_object_get_string(method), rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, mtev_json_object_get_string(method), rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - client_error_count - all|ST[%s,method:%s]",
               agg_tag_string, mtev_json_object_get_string(method));

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - all|ST[%s,method:%s]",
               agg_tag_string, mtev_json_object_get_string(method));

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    }
    if (stat_code >= 500) {

      /* client error count for the service and URL */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - server_error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, mtev_json_object_get_string(method), rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, mtev_json_object_get_string(method),rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - server_error_count - all|ST[%s,method:%s]",
               agg_tag_string, mtev_json_object_get_string(method));

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - all|ST[%s,method:%s]",
               agg_tag_string, mtev_json_object_get_string(method));

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
    }

    /* request count for the service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - %s|ST[%s,method:%s%s]",
             clean_path, agg_tag_string, mtev_json_object_get_string(method),rollup);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - all|ST[%s,method:%s]",
             agg_tag_string, mtev_json_object_get_string(method));
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    /* while we are here, we generate a text metric for the http response code from this transaction */
    if (td->collect_host_level_metrics) {
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - status_code - %s|ST[%s,method:%s%s]",
               clean_path, tag_string, mtev_json_object_get_string(method),rollup);
      snprintf(sc, sizeof(sc), "%d", stat_code);
      update_text(td, team_metrics, metric_name, sc, timestamp);

      /* reuse metric_name for the latency metric name of this metric is the method and the clean_path */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s,method:%s%s]",
               clean_path, tag_string, mtev_json_object_get_string(method),rollup);

      /* round it down to the minute */
      timestamp = ceil_timestamp(timestamp);
      update_histogram(td, team_metrics, metric_name, true, duration_us, timestamp);
    }
  } else if (strcmp(type, "page-load") == 0) {
    /*
     * "page-load"" means that there is a transaction.marks object which tracks http page-load timings.
     *
     * we can't rely on the transaction.name, these often have useless things like
     * "Unknown" as the name
     *
     * Instead, build the name based on the context.page.url
     */

    mtev_json_object *page = mtev_json_object_object_get(context, "page");
    //mtev_json_object *marks = mtev_json_object_object_get(trans, "marks");
    if (page) {
      mtev_json_object *url = mtev_json_object_object_get(page, "url");
      const char *path = mtev_json_object_get_string(url);
      /* chop off protocol://server:port and leave the path */
      const char *prot_sep = strstr(path, "://");
      if (prot_sep) {
        const char *p = strstr(prot_sep + 3, "/");
        if (p) {
          clean_path = genericize_path(p, td);
        } else {
          clean_path = strdup("/");
        }
      } else {
        mtevL(tm_error, "Incoming URL is not http? '%s'\n", path);
        free(clean_path);
        return false;
      }

      /* build aggregate metrics, this replaces host specific info with `all` */
      uint64_t agg_timestamp = ceil_timestamp(timestamp);

      /* average latency for this service and URL */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s,method:GET%s]",
               clean_path, agg_tag_string,rollup);

      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      /* average latency for the service */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - all|ST[%s,method:GET]",
               agg_tag_string);

      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      /* request count for the service and URL */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - %s|ST[%s,method:GET%s]",
               clean_path, agg_tag_string, rollup);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - all|ST[%s,method:GET]",
               agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    }
  } else {
    mtevL(tm_error, "Unknown transaction type: %s\n", type);
  }


  /* check if this transaction has a parent transaction..
   *
   * in the case of a distributed trace, this trans will arrive with a parent.id which references the parent transaction.id
   * it will also be a member of the same trace.id
   *
   * So if this incoming transaction has a parent.id, use the trace.id to find the root transaction.  if we can't find it, republish this
   * transaction and wait 5 minutes to find the root transaction.
   *
   * If this arrives without a parent.id, assume it is root trans and save under trace.id
   */
 process_jaeger:
  {
    if (trace_transaction_hook_invoke(message) == MTEV_HOOK_ABORT) {
      free(clean_path);
      return false;
    }

    bool trace = false;
    mtev_json_object *parent = mtev_json_object_object_get(message, "parent");
    if (parent) {
      tm_transaction_store_add_child(trace_id, strlen(trace_id), message, ttl);
    } else {
      /* this is the root_span */
      const char *thresh_name = NULL;
      if (service_name) {
        thresh_name = mtev_json_object_get_string(service_name);
      }

      uint64_t threshold_us = get_jaeger_threshold_us(thresh_name);
      if (duration_us >= threshold_us && trace == false) {
        mtevL(tm_debug, "Transaction over threshold: %s, %" PRIu64 " > %" PRIu64 "\n", trace_id, duration_us, threshold_us);
        /* process this transaction for jaegerizing */

        /* flag this trans so it's children will be traced */
        trace = true;
      }

      mtev_json_object *apm_t = mtev_json_object_new_object();
      mtev_json_object_object_add(apm_t, "transaction", mtev_json_object_get(message));
      if (clean_path != NULL) {
        mtev_json_object_object_add(apm_t, "url", mtev_json_object_new_string(clean_path));
      } else {
        mtev_json_object_object_add(apm_t, "url", mtev_json_object_new_string("none"));
      }

      tm_transaction_store_entry_t entry;
      entry.first_seen_ms = mtev_now_ms();
      entry.last_modified_ms = mtev_now_ms();
      entry.trace = trace;
      entry.data = apm_t;

      tm_transaction_store_put(trace_id, strlen(trace_id), &entry, ttl);

      /* deal with spans that arrived before the root_span arrived */
      tm_transaction_store_entry_t *children = NULL;
      size_t child_count = tm_transaction_store_get_children(trace_id, strlen(trace_id), &children);
      if (child_count > 0) {
        mtevL(tm_debug, "Transaction %s arrived late, processing %zu children\n", trace_id, child_count);
      }
      for (size_t i = 0; i < child_count; i++) {
        // get type of object.
        mtev_json_object *processor = mtev_json_object_object_get(children[i].data, "processor");
        if (!processor) {
          mtevL(tm_error, "Invalid elastic APM data, missing \"processor\" object\n");
          stats_add64(stats->messages_errored, 1);
          mtev_json_object_put(children[i].data);
          continue;
        }
        const char *event = mtev_json_object_get_string(mtev_json_object_object_get(processor, "event"));
        mtevL(tm_debug, "Processing event: %s\n", event);
        if (strcmp(event, "span") == 0) {
          process_span_message_with_root(children[i].data, &entry);
        } else if (strcmp(event, "error") == 0) {
          process_error_message_with_root(children[i].data, &entry, ttl);
        }
        mtev_json_object_put(children[i].data);
      }
      free(children);
      mtev_json_object_put(entry.data);
    }
    free(clean_path);
  }
  return false;
}
