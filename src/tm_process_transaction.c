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
#include <mtev_b64.h>

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
  char *clean_path = NULL;
  uint64_t timestamp;
  mtev_hash_table *team_metrics = pre_process(message, team, tag_string, &timestamp);
  /**
   * IMPORTANT!
   *
   * The inbound timestamp on an elastic APM transaction document is when the transaction *started*
   * If we record the generated metric data at this timestamp it's misleading and can lead to backfill
   * issues due to aggregation.
   *
   * Consider:
   *
   * A transaction starts at time (t0) and has a duration of 10 minutes ending at t0+10M (t1).  When the transaction completes
   * at t1 we can record the metrics about that transaction (latency, request count).. If we record the metrics at t0 it creates
   * a difficult aggregation issue.  We are aggregating and spitting out metrics from tracemate every minute.  So for all of the
   * requests that start and finish in the minute at t0 we record a total latency.  Then, 10 minutes later, our example
   * request completes at t1.  We have already jettisoned the metrics for t0, to go back and calculate them correctly
   * we would need all that original data at t0.  This is compounded by an unknown potential transaction duration of any limit
   * (i.e. longer than 10 minutes).  The only recourse is to either drop the transaction at t1 (bad), or overwrite the data
   * at t0 with what we know now (also bad since it's incomplete).
   *
   * To combat this we:
   *
   * - Record the metrics at the end of the transaction.  In our example above it would mean that the transaction that started
   * at t0 and ran for 10 minutes would get recorded at t1, not at t0.
   *
   * This is consistent with how logging works.  A load balancer doesn't log out the record until the request ends so it can log the latency.
   *
   * To be most accurate we add the duration to t0 and then for aggregation we center that timestamp within in the minute.
   */
  uint64_t metric_timestamp = timestamp;
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
  metric_timestamp += (duration_us / 1000);

  RETURN_IF_MISSING(trace_json, message, "trace");
  RETURN_IF_MISSING(trace_id_json, trace_json, "id");
  const char *trace_id = mtev_json_object_get_string(trace_id_json);

  const char *service_string = tm_service_name(message);
  if (service_string == NULL) {
    mtevL(tm_error, "'transaction' event is malformed, service name missing\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  uint64_t metric_flush_freq_ms = get_metric_flush_frequency_ms(service_string);

  mtevL(tm_debug, "TRANSACTION for: %s\n\n%s", service_string, mtev_json_object_to_json_string(message));

  /* certain high cardinality metrics can contain a special tag that prevents them from being rolled up
   * for longer term storage
   */
  const char *rollup = "";
  if (td->rollup_high_cardinality == mtev_false) {
    rollup = ",__rollup:false";
  }

  const char *type = mtev_json_object_get_string(mtev_json_object_object_get(trans, "type"));

  if (strcmp(type, "message_read") == 0 || strcmp(type, "messaging") == 0) {

    service_info_t *sit = NULL;
    if (mtev_hash_retrieve(&td->service_data, service_string, strlen(service_string), (void **)&sit)) {
      sit->service_type = SERVICE_TYPE_MESSAGE;
    }

    /* build aggregate metrics, this replaces host specific info with `all` */
    uint64_t agg_timestamp = center_timestamp(metric_timestamp, metric_flush_freq_ms);
    const char *name = mtev_json_object_get_string(mtev_json_object_object_get(trans, "name"));

    /* average latency for this service and transaction name */
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

    service_info_t *sit = NULL;
    if (mtev_hash_retrieve(&td->service_data, service_string, strlen(service_string), (void **)&sit)) {
      sit->service_type = SERVICE_TYPE_SCHEDULED;
    }

    /* build aggregate metrics, this replaces host specific info with `all` */
    uint64_t agg_timestamp = center_timestamp(metric_timestamp, metric_flush_freq_ms);
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

    service_info_t *sit = NULL;
    if (mtev_hash_retrieve(&td->service_data, service_string, strlen(service_string), (void **)&sit)) {
      sit->service_type = SERVICE_TYPE_TRANSACTIONAL;
    }

    /*
     * request means that there is a context.request object
     * We can get the response code from the context.response object as well as the path and synthesize a metric out of it
     *
     * we can't rely on the transaction.name, these often have useless things like
     * "OPTIONS" or "GET" or "*" as the name.
     *
     * Instead, build the name based on the context.request.method, and the context.url.pathname
     */

    mtev_json_object *sampled = mtev_json_object_object_get(trans, "sampled");
    if (mtev_json_object_get_boolean(sampled) == false) {
      mtevL(tm_error, "'transaction' event is malformed, 'sampled' is false, something is misconfigured: %s:%s\n", service_string, trace_id);
      stats_add64(stats->messages_errored, 1);
      return false;
    }

    int mv = tm_apm_server_major_version(message);
    mtev_json_object *context = NULL;
    if (mv == 6) {
      context = mtev_json_object_object_get(message, "context");
      if (context == NULL) {
        mtevL(tm_error, "'transaction' event is malformed, missing 'context' object: %s:%s\n", service_string, trace_id);
        stats_add64(stats->messages_errored, 1);
        return false;
      }
    } else if (mv > 6) {
      context = mtev_json_object_object_get(message, "http");
      if (context == NULL) {
        mtevL(tm_error, "'transaction' event is malformed, missing 'http' object: %s:%s\n", service_string, trace_id);
        stats_add64(stats->messages_errored, 1);
        return false;
      }
    }

    mtev_json_object *req = mtev_json_object_object_get(context, "request");
    mtev_json_object *resp = mtev_json_object_object_get(context, "response");
    if (req == NULL || resp == NULL) {
      mtevL(tm_error, "Transaction missing request or response object: %s:%s\n", service_string, trace_id);
      stats_add64(stats->messages_errored, 1);
      return false;
    }
    mtev_json_object *status_code = mtev_json_object_object_get(resp, "status_code");
    mtev_json_object *method = mtev_json_object_object_get(req, "method");
    mtev_json_object *request_headers = mtev_json_object_object_get(req, "headers");
    mtev_json_object *url = NULL;
    mtev_json_object *path = NULL;
    if (mv == 6) {
      url = mtev_json_object_object_get(req, "url");
      path = mtev_json_object_object_get(url, "pathname");
    } else if (mv > 6) {
      url = mtev_json_object_object_get(message, "url");
      path = mtev_json_object_object_get(url, "path");
    }


    // User-Agent Code
    char *user_agent = NULL;

    // Check if User-Agent is set on the request. If so create metric that is not rolled up - i.e. expires after 4 weeks
    if (request_headers != NULL) {
      mtev_json_object *request_user_agent_obj = mtev_json_object_object_get(request_headers, "User-Agent");
      if (request_user_agent_obj != NULL) {

        mtev_json_object *request_user_agent = mtev_json_object_array_get_idx(request_user_agent_obj,0);
        if (request_user_agent != NULL) {
          const char *temp_user_agent = mtev_json_object_get_string(request_user_agent);
          char *saveptr = NULL;
          if (temp_user_agent != NULL) {

            // Get the fist part of the UA (split by space)
            const char *temp = strtok_r((char *)temp_user_agent, " ", &saveptr);
            if (temp == NULL || strlen(temp) == 0) {
              temp = "Other";
            }

            // Bas64 encode User_agent
            size_t buff_len = mtev_b64_encode_len(strlen(temp)) + 1; // leave space for NUL pointer at end in case whole buffer is used.
            char *buff = (char *)malloc(buff_len);

            int encoded_len = mtev_b64_encode((const unsigned char *)temp, strlen(temp), buff, buff_len); // base64 encode temp into buff
            buff[encoded_len] = '\0';
            user_agent = buff; // Set user_agent to be the base64 encoded string
            // printf("%s - %zu - %s - %s\n", temp, buff_len, buff, user_agent); // This is just here for debug
          }
        }
      }
    }
    // End User-Agent Code

    char METHOD[16] = {0};
    const char *meth = mtev_json_object_get_string(method);
    for (int i = 0; i < strlen(meth); i++) {
      METHOD[i] = toupper(meth[i]);
    }
    if (strcmp(METHOD, "OPTIONS") == 0) {
      stats_add64(stats->messages_filtered, 1);
      return false;
    }

    if (!is_path_ok(mtev_json_object_get_string(path), td)) {
      mtevL(tm_debug, "malformed, path not allowed: %s, %s\n", service_string, mtev_json_object_get_string(path));
      stats_add64(stats->messages_filtered, 1);
      return false;
    }

    int stat_code = mtev_json_object_get_int(status_code);
    /* if (stat_code == 404) { */
    /*   mtevL(tm_debug, "404, not tracking: %s, %s\n", service_string, mtev_json_object_get_string(path)); */
    /*   /\* Don't bother tracking stats on URLs that don't exist *\/ */
    /*   stats_add64(stats->messages_filtered, 1); */
    /*   return false; */
    /* } */

    /* remove any GUIDS or integers from the path to genericize it */
    clean_path = genericize_path(service_string, mtev_json_object_get_string(path), td);

    uint64_t agg_timestamp = center_timestamp(metric_timestamp, metric_flush_freq_ms);

    /* average latency for this service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s,method:%s%s]",
             clean_path, agg_tag_string, METHOD, rollup);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* average latency for the service */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - all|ST[%s,method:%s]",
             agg_tag_string, METHOD);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    if (stat_code >= 400 && stat_code < 500) {
      /* client error count for the service and URL */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - client_error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, METHOD, rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, METHOD, rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - client_error_count - all|ST[%s,method:%s]",
               agg_tag_string, METHOD);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - all|ST[%s,method:%s]",
               agg_tag_string, METHOD);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);


      // Check if User-Agent is set on the request. If so create metric that is not rolled up - i.e. expires after 4 weeks
      if (user_agent != NULL) {

        /* client error count for the service and URL */
        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - client_error_count - %s|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                clean_path, agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - %s|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                clean_path, agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - client_error_count - all|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - all|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      }

    }
    if (stat_code >= 500) {

      /* client error count for the service and URL */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - server_error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, METHOD, rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - %s|ST[%s,method:%s%s]",
               clean_path, agg_tag_string, METHOD,rollup);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - server_error_count - all|ST[%s,method:%s]",
               agg_tag_string, METHOD);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - all|ST[%s,method:%s]",
               agg_tag_string, METHOD);

      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      // Check if User-Agent is set on the request. If so create metric that is not rolled up - i.e. expires after 4 weeks
      if (user_agent != NULL) {
        /* client error count for the service and URL */
        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - server_error_count - %s|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                clean_path, agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - %s|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                clean_path, agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - server_error_count - all|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

        snprintf(metric_name, sizeof(metric_name) - 1, "transaction - error_count - all|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
                agg_tag_string, METHOD, user_agent);

        update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
      }
    }

    /* request count for the service and URL */
    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - %s|ST[%s,method:%s%s]",
             clean_path, agg_tag_string, METHOD,rollup);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - all|ST[%s,method:%s]",
             agg_tag_string, METHOD);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    // Check if User-Agent is set on the request. If so create metric that is not rolled up - i.e. expires after 4 weeks
    if (user_agent != NULL) {
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - request_count - all|ST[%s,method:%s,user_agent:b\"%s\",__rollup:false]",
              agg_tag_string, METHOD, user_agent);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
      free(user_agent);
    }

    /* while we are here, we generate a text metric for the http response code from this transaction */
    if (td->collect_host_level_metrics) {
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - status_code - %s|ST[%s,method:%s%s]",
               clean_path, tag_string, METHOD,rollup);
      snprintf(sc, sizeof(sc), "%d", stat_code);
      update_text(td, team_metrics, metric_name, sc, metric_timestamp);

      /* reuse metric_name for the latency metric name of this metric is the method and the clean_path */
      snprintf(metric_name, sizeof(metric_name) - 1, "transaction - latency - %s|ST[%s,method:%s%s]",
               clean_path, tag_string, METHOD, rollup);

      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);
    }
  } else if (strcmp(type, "page-load") == 0) {

    service_info_t *sit = NULL;
    if (mtev_hash_retrieve(&td->service_data, service_string, strlen(service_string), (void **)&sit)) {
      sit->service_type = SERVICE_TYPE_TRANSACTIONAL;
    }

    /*
     * "page-load"" means that there is a transaction.marks object which tracks http page-load timings.
     *
     * we can't rely on the transaction.name, these often have useless things like
     * "Unknown" as the name
     *
     * Instead, build the name based on the context.page.url
     */

#if 0
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
          clean_path = genericize_path(service_string, p, td);
        } else {
          clean_path = strdup("/");
        }
      } else {
        mtevL(tm_error, "Incoming URL is not http? '%s'\n", path);
        free(clean_path);
        return false;
      }

      /* build aggregate metrics, this replaces host specific info with `all` */
      uint64_t agg_timestamp = center_timestamp(metric_timestamp, metric_flush_freq_ms);

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
#endif
  } else {
    mtevL(tm_debug, "Unknown transaction type: %s\n", type);
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
  if (stats->kafka_lag < 50000) {
    if (trace_transaction_hook_invoke(message) == MTEV_HOOK_ABORT) {
      free(clean_path);
      return false;
    }
  }

  mtev_json_object *parent = mtev_json_object_object_get(message, "parent");
  if (parent) {
    if (stats->kafka_lag < 50000) {
      tm_transaction_store_add_child(trace_id, strlen(trace_id), message, ttl);
    }
  } else {
    uint64_t threshold_us = get_jaeger_threshold_us(service_string);
    if (duration_us >= threshold_us) {
      mtevL(tm_debug, "Transaction over threshold: %s, %" PRIu64 " > %" PRIu64 "\n", trace_id, duration_us, threshold_us);
      /* flag this trans so it and its children will be traced */
      tm_transaction_store_mark_traceable(trace_id, strlen(trace_id));
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
    entry.data = apm_t;

    if (stats->kafka_lag < 50000 || duration_us >= threshold_us) {
      tm_transaction_store_put(trace_id, strlen(trace_id), &entry, ttl);
    }

    mtev_json_object_put(entry.data);
  }
  free(clean_path);
  return false;
}

#undef RETURN_IF_MISSING
