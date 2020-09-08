#include "tm_process.h"

#include "tm_metric.h"
#include "tm_utils.h"
#include "tm_url_squasher.h"

static char *
escape_regex(const char *regex)
{
  size_t ei = 0;
  size_t l = strlen(regex);
  char *escaped = calloc(1, l + 20);
  for(size_t i = 0; i < l; i++) {
    if (regex[i] == '/') {
      memcpy(&escaped[ei], "\\/", 2);
      ei += 2;
    } else {
      escaped[ei] = regex[i];
      ei++;
    }
  }
  return escaped;
}

bool process_regex_message(topic_stats_t *stats, mtev_json_object *message)
{
  char team[128];
  const char *service_name = tm_service_name(message);
  if (!tm_get_team(service_name, team)) {
    mtevL(tm_error, "Failed to get team name\n");
    return false;
  }
  team_data_t *td = get_team_data(team);
  if (td == NULL) {
    mtevL(tm_error, "Failed to get team data\n");
    return false;
  }

  mtev_json_object *r = mtev_json_object_object_get(message, "regex");
  if (r) {
    const char *regex = mtev_json_object_get_string(r);
    const char *replace = mtev_json_object_get_string(mtev_json_object_object_get(message, "replace"));

    mtev_hash_table *regexes = NULL;
    if (!mtev_hash_retrieve(&td->squash_regexes, service_name, strlen(service_name) + 1, (void **)&regexes)) {
      regexes = calloc(1, sizeof(mtev_hash_table));
      mtev_hash_init_locks(regexes, 100, MTEV_HASH_LOCK_MODE_MUTEX);
      mtev_hash_store(&td->squash_regexes, strdup(service_name), strlen(service_name) + 1, regexes);
    }

    pcre_matcher *m = NULL;
    /* unfortunately due to escape issues we have to re-escape the regex string for all forward slashes */
    char *escaped_regex = escape_regex(regex);
    if (!mtev_hash_retrieve(regexes, escaped_regex, strlen(escaped_regex) + 1, (void **)&m)) {

      /* initialize the regex and add it */
      const char *pcre_err;
      int erroff;

      pcre *match = pcre_compile(escaped_regex, 0, &pcre_err, &erroff, NULL);
      if(!match) {
        mtevL(mtev_error, "pcre_compiled failed offset %d: %s on (%s)\n", erroff, pcre_err, escaped_regex);
        free(escaped_regex);
        return false;
      }

      pcre_matcher *pcrem = (pcre_matcher *)calloc(1, sizeof(pcre_matcher));
      pcrem->match = match;
      pcrem->extra = pcre_study(pcrem->match, 0, &pcre_err);
      pcrem->replace = strdup(replace);
      mtev_hash_store(regexes, escaped_regex, strlen(escaped_regex) + 1, pcrem);
    } else {
      free(escaped_regex);
    }
  }
  return false;
}
