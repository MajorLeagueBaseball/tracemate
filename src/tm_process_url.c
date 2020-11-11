#include "tm_process.h"

#include "tm_metric.h"
#include "tm_utils.h"
#include "tm_url_squasher.h"

bool process_url_message(topic_stats_t *stats, mtev_json_object *message)
{
  char team[128];
  /* this is not a traditional elastic apm document */
  mtev_json_object *context = mtev_json_object_object_get(message, "context");
  if (context == NULL) {
    mtevL(tm_error, "Failed to get context from url message\n");
    return false;
  }
  mtev_json_object *service = mtev_json_object_object_get(context, "service");
  if (service == NULL) {
    mtevL(tm_error, "Failed to get service from context\n");
    return false;
  }

  mtev_json_object *name = mtev_json_object_object_get(service, "name");
  if (name == NULL) {
    mtevL(tm_error, "Failed to get name from service\n");
    return false;
  }

  const char *service_name = mtev_json_object_get_string(name);
  if (!tm_get_team(service_name, team)) {
    mtevL(tm_error, "Failed to get team name\n");
    return false;
  }
  team_data_t *td = get_team_data(team);
  if (td == NULL) {
    mtevL(tm_error, "Failed to get team data\n");
    return false;
  }

  service_info_t *sit = NULL;
  if (!mtev_hash_retrieve(&td->service_data, service_name, strlen(service_name), (void **)&sit)) {
    sit = calloc(1, sizeof(service_info_t));
    mtev_hash_store(&td->service_data, strdup(service_name), strlen(service_name), sit);
  }

  mtev_json_object *u = mtev_json_object_object_get(message, "url");
  if (u) {
    const char *url = mtev_json_object_get_string(u);
    tm_path_squasher_t *ps = NULL;
    if (!mtev_hash_retrieve(&td->path_squashers, service_name, strlen(service_name) + 1, (void **)&ps)) {
      ps = tm_path_squasher_alloc(td->path_squash_cardinality_factor);
      mtev_hash_store(&td->path_squashers, strdup(service_name), strlen(service_name) + 1, ps);
    }
    tm_path_squasher_add_path(ps, url);
  }
  return false;
}
