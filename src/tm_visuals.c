#include "tm_circonus.h"
#include "tm_visuals.h"
#include "tm_utils.h"

#include <mtev_dyn_buffer.h>

#include <curl/curl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

static bool
service_dash_exists(team_data_t *td, const char *service_name, const service_info_t *service_info)
{
  mtev_dyn_buffer_t response;
  bool exists = false;

  //if (service_info->created) return true;

  mtev_dyn_buffer_init(&response);

  /* query circonus for the dashboard */
  if (circonus_curl_get(td->circonus_api_key, &response, "https://api.circonus.com/v2/dashboard?search=%s%%20-%%20Performance*", service_name)) {
    mtev_json_tokener *tok = mtev_json_tokener_new();
    mtev_json_object *o = mtev_json_tokener_parse_ex(tok, (const char *)mtev_dyn_buffer_data(&response), mtev_dyn_buffer_used(&response));
    mtev_json_tokener_free(tok);
    if (o != NULL) {
      exists = mtev_json_object_array_length(o) >= 1;
      if (exists) {
        mtevL(mtev_notice, "Service dash for: %s already exists\n", service_name);
      }
      mtev_json_object_put(o);
    }
  } 
  mtev_dyn_buffer_destroy(&response);
  return exists;
}

static char *
maybe_create_graph(team_data_t *td, const char *path, mtev_hash_table *values)
{
  char *uuid = NULL;
  /* do values replacement in the file at 'path' */
  int fd = open(path, 'r');
  if (fd > 0) {
    mtev_dyn_buffer_t contents;
    mtev_dyn_buffer_init(&contents);
    mtev_dyn_buffer_ensure(&contents, 4096);
    ssize_t r = 0;
    while((r = read(fd, mtev_dyn_buffer_write_pointer(&contents), 4096)) > 0) {
      mtev_dyn_buffer_advance(&contents, r);
      mtev_dyn_buffer_ensure(&contents, 4096);
    }
    close(fd);
    char eof[1] = {0};
    mtev_dyn_buffer_add(&contents, eof, 1);
    char *resolved = tm_replace_vars((const char *)mtev_dyn_buffer_data(&contents), values);
    mtev_dyn_buffer_destroy(&contents);
    
    /* parse the resolved json and get the graph title */
    mtev_json_object *o = mtev_json_tokener_parse(resolved);
    if (o == NULL) {
      mtevL(mtev_error, "Resolved json from %s is unparseable\n", path);
      free(resolved);
      goto bail;
    }
    mtev_json_object *t = mtev_json_object_object_get(o, "title");
    const char *title = mtev_json_object_get_string(t);

    /* query circonus to see if this graph exists */
    mtev_dyn_buffer_t response;
    mtev_dyn_buffer_init(&response);
    CURL *c = curl_easy_init();
    char *escaped_title = curl_easy_escape(c, title, 0);
    curl_easy_cleanup(c);

    bool need_create = true;
    if (circonus_curl_get(td->circonus_api_key, &response, "https://api.circonus.com/v2/graph?search=%s*", escaped_title)) {
      mtev_json_tokener *tok = mtev_json_tokener_new();
      mtev_json_object *o2 = mtev_json_tokener_parse_ex(tok, (const char *)mtev_dyn_buffer_data(&response), mtev_dyn_buffer_used(&response));
      mtev_json_tokener_free(tok);
      if (o2 != NULL) {
        for (int i = 0; i < mtev_json_object_array_length(o2); i++) {
          mtev_json_object *graph = mtev_json_object_array_get_idx(o2, i);
          mtev_json_object *t = mtev_json_object_object_get(graph, "title");
          const char *tt = mtev_json_object_get_string(t);
          if (strcmp(tt, title) == 0) {
            mtev_json_object *cid = mtev_json_object_object_get(graph, "_cid");
            const char *cc = mtev_json_object_get_string(cid);
            mtevL(mtev_notice, "Graph '%s' exists '%s': %s\n", path, title, cc);
            uuid = strdup(cc+7);
            need_create = false;
          }
        }
        mtev_json_object_put(o2);
      }
    }
    if (need_create) {
      mtev_dyn_buffer_reset(&response);
      if (circonus_curl_post(td->circonus_api_key, &response, "https://api.circonus.com/v2/graph", resolved)) {
        mtev_json_tokener *tok = mtev_json_tokener_new();
        mtev_json_object *o2 = mtev_json_tokener_parse_ex(tok, (const char *)mtev_dyn_buffer_data(&response), mtev_dyn_buffer_used(&response));
        mtev_json_tokener_free(tok);
        if (o2 != NULL) {
          mtev_json_object *cid = mtev_json_object_object_get(o2, "_cid");
          const char *cc = mtev_json_object_get_string(cid);
          uuid = strdup(cc+7);
          mtevL(mtev_notice, "Created '%s' graph '%s': %s\n", path, title, uuid);
          mtev_json_object_put(o2);
        }
      }
    }
    free(resolved);
    mtev_json_object_put(o);
    mtev_dyn_buffer_destroy(&response);
    curl_free(escaped_title);
    return uuid;
  } else {
    mtevL(mtev_error, "Error opening: %s\n", path);
  }
 bail:
  return strdup("00000000-0000-0000-0000-000000000000");
}

static bool
create_dashboard(team_data_t *td, const char *path, mtev_hash_table *values)
{
  /* do values replacement in the file at 'path' */
  int fd = open(path, 'r');
  if (fd > 0) {
    mtev_dyn_buffer_t contents;
    mtev_dyn_buffer_init(&contents);
    mtev_dyn_buffer_ensure(&contents, 4096);
    ssize_t r = 0;
    while((r = read(fd, mtev_dyn_buffer_write_pointer(&contents), 4096)) > 0) {
      mtev_dyn_buffer_advance(&contents, r);
      mtev_dyn_buffer_ensure(&contents, 4096);
    }
    close(fd);
    char eof[1] = {0};
    mtev_dyn_buffer_add(&contents, eof, 1);
    char *resolved = tm_replace_vars((const char *)mtev_dyn_buffer_data(&contents), values);
    mtev_dyn_buffer_reset(&contents);
    if (circonus_curl_post(td->circonus_api_key, &contents, "https://api.circonus.com/v2/dashboard", resolved)) {
      mtevL(mtev_notice, "Created dashboard\n");
      mtev_dyn_buffer_destroy(&contents);
      free(resolved);
      return true;
    }
    mtev_dyn_buffer_destroy(&contents);
    free(resolved);
  }
  return false;
}

void
tm_create_visuals(mtev_hash_table *teams)
{
  char service_name[256];
  char threshold_ms[25];

  mtev_hash_iter it = MTEV_HASH_ITER_ZERO;
  while(mtev_hash_adv(teams, &it)) {
    team_data_t *td = (team_data_t *)it.value.ptr;
    const char *team = it.key.str;
    mtevL(tm_notice, "Creating visuals for team: %s\n", team);

    /* loop through the service_data */
    mtev_hash_iter service_it = MTEV_HASH_ITER_ZERO;
    while(mtev_hash_adv(&td->service_data, &service_it)) {
      sprintf(service_name, "%.*s", service_it.klen, service_it.key.str);
      service_info_t *service_info = (service_info_t *)service_it.value.ptr;
      if (service_info->service_agent == NULL || service_info->service_type == SERVICE_TYPE_UNKNOWN) {
        continue;
      }

      sleep(1); // throttle the requests to circonus, this only runs every 10 minutes so there is no rush

      mtev_hash_table values;
      mtev_hash_init_locks(&values, 50, MTEV_HASH_LOCK_MODE_NONE);
      mtev_hash_store(&values, "check_id", strlen("check_id"), strdup(td->check_id));
      mtev_hash_store(&values, "slo_budget_limit", strlen("slo_budget_limit"), strdup("0.005"));
      mtev_hash_store(&values, "slo_budget_limit_as_percent", strlen("slo_budget_limit_as_percent"), strdup("0.5"));
      mtev_hash_store(&values, "slo_budget_length", strlen("slo_budget_length"), strdup("4w"));
      mtev_hash_store(&values, "top_count", strlen("top_count"), strdup("15"));

      /* use the jaeger threshold as the latency SLO level 
       * Things that are interesting enough to trace are SLO violators? 
       */
      uint64_t threshold = get_jaeger_threshold_us(service_name);
      threshold = threshold / 1000; // to milliseconds
      sprintf(threshold_ms, "%" PRIu64, threshold);
      mtev_hash_store(&values, "slo_ms", strlen("slo_ms"), strdup(threshold_ms));
      threshold = threshold / 1000; // to seconds
      sprintf(threshold_ms, "%" PRIu64, threshold);
      mtev_hash_store(&values, "slo_secs", strlen("slo_secs"), strdup(threshold_ms));

      if (!service_dash_exists(td, service_name, service_info)) {
        mtev_hash_store(&values, "service_name", strlen("service_name"), strdup(service_name));

        char *average_response_graph_uuid = NULL;
        if (service_info->service_type == SERVICE_TYPE_TRANSACTIONAL) {
          average_response_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/average_response_time_graph.json", &values);
        } else {
          average_response_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/average_response_time_graph_message.json", &values);
        }
        mtev_hash_store(&values, "average_response_time_graph_uuid", strlen("average_response_time_graph_uuid"),
                        average_response_graph_uuid);

        char *throughput_graph_uuid = NULL;
        if (service_info->service_type == SERVICE_TYPE_TRANSACTIONAL) {
          throughput_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/throughput_graph.json", &values);
        } else {
          throughput_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/throughput_graph_message.json", &values);
        }
        mtev_hash_store(&values, "throughput_graph_uuid", strlen("throughput_graph_uuid"),
                        throughput_graph_uuid);

        char *error_rate_graph_uuid = NULL;
        if (service_info->service_type == SERVICE_TYPE_TRANSACTIONAL) {
          error_rate_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/error_rate_graph.json", &values);
        } else {
          error_rate_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/error_rate_graph_message.json", &values);
        }
        mtev_hash_store(&values, "error_rate_graph_uuid", strlen("error_rate_graph_uuid"),
                        error_rate_graph_uuid);

        char *heatmap_response_time_graph_uuid = NULL;
        if (service_info->service_type == SERVICE_TYPE_TRANSACTIONAL) {
          heatmap_response_time_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/heatmap_response_time.json", &values);
        } else {
          heatmap_response_time_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/heatmap_response_time_message.json", &values);
        }
        mtev_hash_store(&values, "heatmap_response_time_graph_uuid", strlen("heatmap_response_time_graph_uuid"),
                        heatmap_response_time_graph_uuid);

        char *response_error_budget_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/response_error_budget.json", &values);
        mtev_hash_store(&values, "response_error_budget_graph_uuid", strlen("response_error_budget_graph_uuid"),
                        response_error_budget_graph_uuid);
        char *cpu_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/cpu_graph.json", &values);
        mtev_hash_store(&values, "cpu_graph_uuid", strlen("cpu_graph_uuid"),
                        cpu_graph_uuid);
        char *latency_error_budget_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/latency_error_budget.json", &values);
        mtev_hash_store(&values, "latency_error_budget_graph_uuid", strlen("latency_error_budget_graph_uuid"),
                        latency_error_budget_graph_uuid);
        char *topn_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/topn_graph.json", &values);
        mtev_hash_store(&values, "topn_graph_uuid", strlen("topn_graph_uuid"),
                        topn_graph_uuid);

        char *memory_graph_uuid = NULL;
        if (strcmp(service_info->service_agent, "java") == 0) {
          memory_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/jvm_memory_graph.json", &values);
        } else if (strcmp(service_info->service_agent, "nodejs") == 0) {
          memory_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/nodejs_memory_graph.json", &values);
        } else if (strcmp(service_info->service_agent, "python") == 0) {
          memory_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/python_memory_graph.json", &values);
        } else if (strcmp(service_info->service_agent, "go") == 0) {
          memory_graph_uuid = maybe_create_graph(td, "/tracemate/visuals/golang_memory_graph.json", &values);
        }
        mtev_hash_store(&values, "memory_graph_uuid", strlen("memory_graph_uuid"),
                        memory_graph_uuid);

        service_info->created = create_dashboard(td, "/tracemate/visuals/dashboard_with_slo.json", &values);
      } else {
        service_info->created = true;
      }
      mtev_hash_destroy(&values, NULL, free);
    }
  }
}
