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

#include <mtev_defines.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include <openssl/md5.h>

#include <mtev_main.h>
#include <eventer/eventer.h>
#include <mtev_dyn_buffer.h>
#include <mtev_b64.h>
#include <mtev_memory.h>
#include <mtev_log.h>
#include <mtev_hash.h>
#include <mtev_security.h>
#include <mtev_watchdog.h>
#include <mtev_lockfile.h>
#include <mtev_listener.h>
#include <mtev_dso.h>
#include <mtev_console.h>
#include <mtev_rest.h>
#include <mtev_reverse_socket.h>
#include <mtev_capabilities_listener.h>
#include <mtev_heap_profiler.h>
#include <mtev_conf.h>
#include <mtev_events_rest.h>
#include <mtev_stats.h>

#include <curl/curl.h>

#include "tm_circonus.h"
#include "tm_hooks.h"
#include "tm_kafka.h"
#include "tm_log.h"
#include "tm_metric.h"
#include "tm_process.h"
#include "tm_transaction_store.h"
#include "tm_url_squasher.h"
#include "tm_utils.h"
#include "tm_version.h"
#include "tm_visuals.h"

#define APPNAME "tm"
#define CHILD_WATCHDOG_TIMEOUT 5 /*seconds*/

static char *config_file = NULL;
static char *droptouser = NULL;
static char *droptogroup = NULL;
static char *chrootpath = NULL;
static char *infra_dest_url = NULL;
static int foreground = 0;
static int debug = 0;
static char *glider = NULL;
static mtev_hash_table threshold_hash;
static mtev_hash_table flushes_hash;
static uint64_t default_jaeger_threshold_us = 2000 * 1000; // ms to us
static uint64_t default_metric_flush_freq_ms = 60 * 1000; 
static tm_kafka_t *kafka_conn = NULL;
static tm_kafka_t *kafka_producer_conn = NULL;
static tm_kafka_topic_t *agg_topic = NULL;
static tm_kafka_topic_t *url_topic = NULL;
static tm_kafka_topic_t *regex_topic = NULL;
static int transaction_lookback_secs = 300;
static eventer_jobq_t *metric_flush_jobq;
static eventer_jobq_t *transaction_clean_jobq;
static eventer_jobq_t *maintenance_jobq;
static eventer_jobq_t *visuals_jobq;
static stats_recorder_t *global_stats_recorder;

static size_t all_stats_len = 0;
static topic_stats_t **all_stats = NULL;
static bool test = false;

static void usage(const char *progname) {
  printf("Usage for %s:\n", progname);
  return;
}

void parse_clargs(int argc, char **argv) {
  int c;
  while((c = getopt(argc, argv, "hc:dDu:g:n:t:l:L:G:T")) != EOF) {
    switch(c) {
    case 'T':
      test = true;
      break;
    case 'G':
      free(glider);
      glider = strdup(optarg);
      break;
    case 'h':
      usage(argv[0]);
      exit(1);
      break;
    case 'l':
      mtev_main_enable_log(optarg);
      break;
    case 'L':
      mtev_main_disable_log(optarg);
      break;
    case 'n':
      {
        char *cp = optarg ? strchr(optarg, ':') : NULL;
        if(!cp) mtev_listener_skip(optarg, 0);
        else {
          if(cp == optarg) optarg = NULL;
          *cp++ = '\0';
          mtev_listener_skip(optarg, atoi(cp));
        }
      }
      break;
    case 'u':
      free(droptouser);
      droptouser = strdup(optarg);
      break;
    case 'g':
      free(droptogroup);
      droptogroup = strdup(optarg);
      break;
    case 't':
      free(chrootpath);
      chrootpath = strdup(optarg);
      break;
    case 'c':
      free(config_file);
      config_file = strdup(optarg);
      break;
    case 'D':
      foreground++;
      break;
    case 'd':
      debug++;
      break;
    default:
      break;
    }
  }
}

static int __reload_needed = 0;
static void request_conf_reload(int sig) {
  if(sig == SIGHUP) {
    __reload_needed = 1;
  }
}
static int notice_hup(eventer_t e, int mask, void *unused, struct timeval *now) {
  if(__reload_needed) {
    mtevL(tm_error, "SIGHUP received, performing reload\n");
    if(mtev_conf_load(config_file) == -1) {
      mtevL(tm_error, "Cannot load config: '%s'\n", config_file);
      exit(-1);
    }
    __reload_needed = 0;
  }
  return 0;
}

void
tm_init_globals(void)
{
  tm_metrics_init();
}

static int
kafka_read(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  topic_stats_t *t = (topic_stats_t *)closure;
  switch(mask) {
  case EVENTER_ASYNCH_WORK:{
    tm_kafka_consume_messages(t);
    break;
  }
  case EVENTER_ASYNCH:
    tm_kafka_get_high_watermark(t->topic, &t->high_watermark);
    if (t->kafka_offset > 0 && t->high_watermark > t->kafka_offset) {
      t->kafka_lag = t->high_watermark - t->kafka_offset;
    } 
    mtevL(tm_debug, "Kafka lag: %" PRIu64 "\n", t->kafka_lag);

    tm_kafka_poll(kafka_conn);
    tm_kafka_poll(kafka_producer_conn);

    /* enqueue another read */
    if (t->read_delay_ms == 0) {
      eventer_t enew = eventer_alloc_asynch(kafka_read, t);
      eventer_add_asynch(t->kafka_read_jobq, enew);
    }
    break;
  };

  return 0;
}

static int
metric_flush(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  mtev_hash_table *teams = (mtev_hash_table *)closure;
  switch(mask)
    {
    case EVENTER_ASYNCH_WORK:
      {
        tm_flush_team_metrics(teams, agg_topic, regex_topic);
      }
      break;
    case EVENTER_ASYNCH:
      break;
    };
  return 0;
}

static int
visuals_create(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  mtev_hash_table *teams = (mtev_hash_table *)closure;
  switch(mask)
    {
    case EVENTER_ASYNCH_WORK:
      {
        tm_create_visuals(teams);
      }
      break;
    case EVENTER_ASYNCH:
      break;
    };
  return 0;
}

static int
metric_trigger_flush(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  /* trigger the flush jobq */
  eventer_t enew = eventer_alloc_asynch(metric_flush, closure);
  eventer_add_asynch(metric_flush_jobq, enew);

  /* retrigger myself in N seconds */
  struct timeval tvnew;
  mtev_gettimeofday(&tvnew, NULL);
  tvnew.tv_sec += 10;
  eventer_add_at(metric_trigger_flush, closure, tvnew);
  return 0;
}

static int
transaction_clean(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  switch(mask) {
    case EVENTER_ASYNCH_WORK: {
      /* iterate the values and anything not seen in lookback ms should be flushed to 
       * jaeger if it's flagged for trace
       */
      while(true) {
        size_t jaegered_count = tm_transaction_store_process_jaeger();
        mtevL(tm_error, "Jaegered %zu transactions\n", jaegered_count);

        size_t loop_count = tm_transaction_store_delete_old_transactions();
        mtevL(tm_error, "Cleaned %zu transactions\n", loop_count);
        if (loop_count < 500) break;
      }
      break;
    }
  case EVENTER_ASYNCH:
    break;
  };
  return 0;
}

static int
trigger_transaction_cleanup(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  /* trigger the flush jobq */
  eventer_t enew = eventer_alloc_asynch(transaction_clean, closure);
  if (eventer_try_add_asynch(transaction_clean_jobq, enew) == mtev_false) {
    eventer_free(enew);
  }

  /* retrigger myself in N seconds */
  struct timeval tvnew;
  mtev_gettimeofday(&tvnew, NULL);
  tvnew.tv_sec += 3;
  eventer_add_at(trigger_transaction_cleanup, closure, tvnew);
  return 0;
}


static int
maintenance_function(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  mtev_hash_table *thresholds = &threshold_hash;
  mtev_hash_table *flushes = &flushes_hash;
  switch(mask) {
    case EVENTER_ASYNCH_WORK: {

      threshold_fetch_hook_invoke(thresholds);
      metric_flush_frequency_fetch_hook_invoke(flushes);

      send_infra_metrics(infra_dest_url, global_stats_recorder);

      break;
    }
  case EVENTER_ASYNCH:
    break;
  };
  return 0;
}

static int
trigger_visuals(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  /* trigger the visuals jobq */
  eventer_t enew = eventer_alloc_asynch(visuals_create, closure);
  eventer_add_asynch(visuals_jobq, enew);

  /* retrigger myself in N seconds */
  struct timeval tvnew;
  mtev_gettimeofday(&tvnew, NULL);
  tvnew.tv_sec += 600; // every 10 minutes
  eventer_add_at(trigger_visuals, closure, tvnew);
  return 0;
}


uint64_t get_jaeger_threshold_us(const char *service_name)
{
  if (service_name != NULL) {
    const char *data = NULL;
    if (mtev_hash_retr_str(&threshold_hash, service_name, strlen(service_name), &data)) {
      uint64_t x = strtoull(data, NULL, 10);
      return x * 1000L;
    } else if (mtev_hash_retr_str(&threshold_hash, "default", strlen("default"), &data)) {
      uint64_t x = strtoull(data, NULL, 10);
      return x * 1000L;
    }
  }
  return default_jaeger_threshold_us;
}

uint64_t get_metric_flush_frequency_ms(const char *service_name)
{
  if (service_name != NULL) {
    const char *data = NULL;
    if (mtev_hash_retr_str(&flushes_hash, service_name, strlen(service_name), &data)) {
      uint64_t x = strtoull(data, NULL, 10);
      return x;
    } else if (mtev_hash_retr_str(&flushes_hash, "default", strlen("default"), &data)) {
      uint64_t x = strtoull(data, NULL, 10);
      return x;
    }
  }
  return default_metric_flush_freq_ms;
}

static int
trigger_maintenance(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  /* trigger the flush jobq */
  eventer_t enew = eventer_alloc_asynch(maintenance_function, closure);
  eventer_add_asynch(maintenance_jobq, enew);

  /* retrigger myself in N seconds */
  struct timeval tvnew;
  mtev_gettimeofday(&tvnew, NULL);
  tvnew.tv_sec += 60;
  eventer_add_at(trigger_maintenance, closure, tvnew);
  return 0;
}

static int 
kafka_trigger_read(eventer_t e, int mask, void *closure, struct timeval *tv)
{
  topic_stats_t *stats = (topic_stats_t *)closure;

  eventer_t newe = eventer_alloc_asynch(kafka_read, stats);
  eventer_add_asynch(stats->kafka_read_jobq, newe);

  eventer_add_in_s_us(kafka_trigger_read, stats, stats->read_delay_ms / 1000, (stats->read_delay_ms % 1000) * 1000);
  return 0;
}

static void
tm_init()
{
  metric_flush_jobq = eventer_jobq_create("tm_metric_flush_jobq");
  eventer_jobq_set_min_max(metric_flush_jobq, 1, 4);
  transaction_clean_jobq = eventer_jobq_create("tm_transaction_clean_jobq");
  eventer_jobq_set_max_backlog(transaction_clean_jobq, 2);
  maintenance_jobq = eventer_jobq_create("tm_maintenance_jobq");
  mtev_hash_init_locks(&threshold_hash, 200, MTEV_HASH_LOCK_MODE_MUTEX);
  mtev_hash_init_locks(&flushes_hash, 200, MTEV_HASH_LOCK_MODE_MUTEX);

  /* invoke the flush frequency hook to load any services that have non-default flush frequency 
   * 
   * We need to do this early so we don't use an incorrect frequency for a service before the real
   * frequency is loaded
   */
  metric_flush_frequency_fetch_hook_invoke(&flushes_hash);

  visuals_jobq = NULL;
  mtev_boolean _tm_create_visuals = mtev_false;
  mtev_conf_get_boolean(MTEV_CONF_ROOT, "/tm/create_visuals", &_tm_create_visuals);
  if (_tm_create_visuals) {
    visuals_jobq = eventer_jobq_create("tm_visuals_jobq");
  }

  /* init the transaction store */
  char *tdb_path = "/tracemate/data/ts";
  uint64_t db_size = 5000000000;
  mtev_boolean bloom_filter = mtev_false;
  mtev_conf_get_int32(MTEV_CONF_ROOT, "/tm/transaction_db/@lookback_seconds", &transaction_lookback_secs);
  mtev_conf_get_string(MTEV_CONF_ROOT, "/tm/transaction_db/@path", &tdb_path);
  mtev_conf_get_uint64(MTEV_CONF_ROOT, "/tm/transaction_db/@initial_size", &db_size);
  mtev_conf_get_boolean(MTEV_CONF_ROOT, "/tm/transaction_db/@use_bloom_filter", &bloom_filter);
  tm_transaction_store_init(TM_TRANSACTION_STORE_TYPE_LMDB, tdb_path, db_size, transaction_lookback_secs, bloom_filter);

  /* read mtev_config to determine what topic and partition I am reading */
  char *broker_list = NULL;
  char *group_id = NULL;
  int batch_size = 100;

  if (!mtev_conf_get_string(MTEV_CONF_ROOT, "/tm/kafka/broker_list", &broker_list)) {
    mtevFatal(tm_error, "A broker_list is required to read from kafka, exiting");
  }
  if (!mtev_conf_get_string(MTEV_CONF_ROOT, "/tm/kafka/group_id", &group_id)) {
    mtevFatal(tm_error, "A group_id is required to save offsets, exiting");
  }

  /* create the kafka connections */
  kafka_conn = tm_kafka_connect(broker_list, RD_KAFKA_CONSUMER, group_id);
  kafka_producer_conn = tm_kafka_connect(broker_list, RD_KAFKA_PRODUCER, group_id);
  free(broker_list);
  free(group_id);


  if (kafka_conn == NULL || kafka_producer_conn == NULL) {
    mtevFatal(tm_error, "Failed to create tm_kafka_t, exiting");
  }

  if (global_stats_recorder == NULL) {
    global_stats_recorder = stats_recorder_alloc();
  }

  agg_topic = tm_kafka_produce_topic(kafka_producer_conn, "tracemate_aggregates");
  url_topic = tm_kafka_produce_topic(kafka_producer_conn, "tracemate_urls");
  regex_topic = tm_kafka_produce_topic(kafka_producer_conn, "tracemate_regexes");

  init_path_regex(url_topic);

  stats_ns_t *root_ns = stats_recorder_global_ns(global_stats_recorder);
  stats_ns_t *tm_ns = stats_register_ns(global_stats_recorder, root_ns, "tracemate");

  int topic_count = 0;
  mtev_conf_section_t *topics = mtev_conf_get_sections_read(MTEV_CONF_ROOT, "/tm/kafka/topics/topic", &topic_count);
  all_stats_len = topic_count;
  all_stats = (topic_stats_t **)calloc(all_stats_len, sizeof(topic_stats_t *));
  for (int i = 0; i < topic_count; i++) {
    char *topic = NULL;
    int32_t partition, read_delay_ms = 0;
    int64_t offset = RD_KAFKA_OFFSET_STORED;
    mtev_conf_get_string(topics[i], "@name", &topic);
    mtev_conf_get_int32(topics[i], "@partition", &partition);
    mtev_conf_get_int32(topics[i], "@batch_size", &batch_size);
    mtev_conf_get_int32(topics[i], "@read_delay_ms", &read_delay_ms);

    /* Either an actual offset [0..N] or the integer value of one of the predefined offset types:
     *
     * #define 	RD_KAFKA_OFFSET_BEGINNING   -2
     * #define 	RD_KAFKA_OFFSET_END   -1
     * #define 	RD_KAFKA_OFFSET_STORED   -1000
     * #define 	RD_KAFKA_OFFSET_TAIL(CNT)   (RD_KAFKA_OFFSET_TAIL_BASE - (CNT))
     *
     * see: https://docs.confluent.io/2.0.0/clients/librdkafka/rdkafka_8h.html#ae21dcd2d8c6195baf7f9f4952d7e12d4
     */
    mtev_conf_get_int64(topics[i], "@offset", &offset);

    /* start consumption */
    all_stats[i] = (topic_stats_t *)calloc(1, sizeof(topic_stats_t));

    all_stats[i]->topic = tm_kafka_start_consume(kafka_conn, topic, partition, batch_size, offset);
    all_stats[i]->read_delay_ms = read_delay_ms;
    if (all_stats[i]->topic == NULL) {
      mtevFatal(tm_error, "Failed to start consumption, exiting");
    }
    char part[20];
    snprintf(part, sizeof(part), "%d", partition);
    stats_ns_t *tns = stats_register_ns(global_stats_recorder, tm_ns, topic);
    stats_ns_t *ns = stats_register_ns(global_stats_recorder, tns, part);

    mtevAssert(ns != NULL);
    all_stats[i]->messages_seen = stats_register(ns, "messages_seen", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->messages_seen, "partition", part);
    stats_handle_add_tag(all_stats[i]->messages_seen, "topic", topic);
    all_stats[i]->messages_processed = stats_register(ns, "messages_processed", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->messages_processed, "partition", part);
    stats_handle_add_tag(all_stats[i]->messages_processed, "topic", topic);
    all_stats[i]->messages_errored = stats_register(ns, "messages_errored", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->messages_errored, "partition", part);
    stats_handle_add_tag(all_stats[i]->messages_errored, "topic", topic);
    all_stats[i]->messages_orphaned = stats_register(ns, "messages_orphaned", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->messages_orphaned, "partition", part);
    stats_handle_add_tag(all_stats[i]->messages_orphaned, "topic", topic);
    all_stats[i]->messages_filtered = stats_register(ns, "messages_filtered", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->messages_filtered, "partition", part);
    stats_handle_add_tag(all_stats[i]->messages_filtered, "topic", topic);
    all_stats[i]->metrics_flushed = stats_register(ns, "metrics_flushed", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->metrics_flushed, "partition", part);
    stats_handle_add_tag(all_stats[i]->metrics_flushed, "topic", topic);
    all_stats[i]->unknown_team = stats_register(ns, "unknown_team", STATS_TYPE_COUNTER);
    stats_handle_add_tag(all_stats[i]->unknown_team, "partition", part);
    stats_handle_add_tag(all_stats[i]->unknown_team, "topic", topic);
    all_stats[i]->kafka_offset_rob = stats_register(ns, "kafka_offset", STATS_TYPE_UINT64);
    stats_observe(all_stats[i]->kafka_offset_rob, STATS_TYPE_UINT64, &all_stats[i]->kafka_offset);
    stats_handle_add_tag(all_stats[i]->kafka_offset_rob, "partition", part);
    stats_handle_add_tag(all_stats[i]->kafka_offset_rob, "topic", topic);

    all_stats[i]->high_watermark_rob = stats_register(ns, "kafka_high_watermark", STATS_TYPE_INT64);
    stats_observe(all_stats[i]->high_watermark_rob, STATS_TYPE_INT64, &all_stats[i]->high_watermark);
    stats_handle_add_tag(all_stats[i]->high_watermark_rob, "partition", part);
    stats_handle_add_tag(all_stats[i]->high_watermark_rob, "topic", topic);

    all_stats[i]->kafka_lag_rob = stats_register(ns, "kafka_lag", STATS_TYPE_INT64);
    stats_observe(all_stats[i]->kafka_lag_rob, STATS_TYPE_INT64, &all_stats[i]->kafka_lag);
    stats_handle_add_tag(all_stats[i]->kafka_lag_rob, "partition", part);
    stats_handle_add_tag(all_stats[i]->kafka_lag_rob, "topic", topic);


    all_stats[i]->message_process_latency = stats_register(ns, "message_process_latency", STATS_TYPE_HISTOGRAM);
    stats_handle_units(all_stats[i]->message_process_latency, "seconds");
    stats_handle_add_tag(all_stats[i]->message_process_latency, "partition", part);
    stats_handle_add_tag(all_stats[i]->message_process_latency, "topic", topic);

    char read_jobq_name[256];
    snprintf(read_jobq_name, sizeof(read_jobq_name) - 1, "%s-%s", part, topic);
    all_stats[i]->kafka_read_jobq = eventer_jobq_create(read_jobq_name);
    char process_jobq_name[256];
    snprintf(process_jobq_name, sizeof(process_jobq_name) - 1, "%s-process", topic);
    all_stats[i]->kafka_process_jobq = eventer_jobq_create(process_jobq_name);
    // don't allow more than 1000 unprocessed messages
    eventer_jobq_set_max_backlog(all_stats[i]->kafka_process_jobq, 1000);
    eventer_jobq_set_min_max(all_stats[i]->kafka_process_jobq, 1, 4);
    eventer_jobq_set_concurrency(all_stats[i]->kafka_process_jobq, 4);
    if (all_stats[i]->read_delay_ms == 0) {
      /*
       * trigger a read job in the read jobq for each topic
       * The read jobq will retrigger when it's done
       * to create a continuous read loop
       */
      eventer_t e = eventer_alloc_asynch(kafka_read, all_stats[i]);
      eventer_add_asynch(all_stats[i]->kafka_read_jobq, e);
    } else {
      eventer_add_in_s_us(kafka_trigger_read, all_stats[i], all_stats[i]->read_delay_ms / 1000, (all_stats[i]->read_delay_ms % 1000) * 1000);
    }
    free(topic);
  }
  mtev_conf_release_sections_read(topics, topic_count);

  char *journal_path = "/tracemate/data/journal";
  mtev_conf_get_string(MTEV_CONF_ROOT, "/tm/circonus_journal_path", &journal_path);
  bool ok = tm_circonus_init(journal_path);
  mtevAssert(ok);
  free(tdb_path);


  /* process teams */
  int team_count = 0;
  mtev_conf_section_t *teams = mtev_conf_get_sections_read(MTEV_CONF_ROOT, "/tm/teams/team", &team_count);
  for (int i = 0; i < team_count; i++) {
    char *name = NULL;
    team_data_t *td = (team_data_t *)calloc(1, sizeof(team_data_t));
    /* by default we don't want to collect ip/host cardinality metrics */
    td->collect_host_level_metrics = mtev_false;
    td->collect_host_level_system = mtev_false;
    td->rollup_high_cardinality = mtev_false;

    mtev_conf_get_string(teams[i], "@name", &name);
    mtev_conf_get_string(teams[i], "@metric_submission_url", &td->metric_submission_url);
    mtev_conf_get_string(teams[i], "@jaeger_dest_url", &td->jaeger_dest_url);
    mtev_conf_get_boolean(teams[i], "@collect_host_level_metrics", &td->collect_host_level_metrics);
    mtev_conf_get_boolean(teams[i], "@collect_host_level_system", &td->collect_host_level_system);
    mtev_conf_get_boolean(teams[i], "@rollup_high_cardinality", &td->rollup_high_cardinality);
    mtev_conf_get_string(teams[i], "@circonus_api_key", &td->circonus_api_key);
    mtev_conf_get_string(teams[i], "@check_id", &td->check_id);

    /* allow url list */
    td->allowlist_count = 0;
    mtev_conf_section_t *allowlist_sections = mtev_conf_get_sections_read(teams[i], "self::node()/path_allowlist/path", (int *)&td->allowlist_count);
    if (allowlist_sections != NULL) {
      td->allowlist = (char **)calloc(td->allowlist_count, sizeof(char *));

      for (int i = 0; i < td->allowlist_count; i++) {
        char *wl = NULL;
        if (mtev_conf_get_string(allowlist_sections[i], "self::node()", &wl) && wl != NULL) {
          td->allowlist[i] = wl;
        }
      }
      mtev_conf_release_sections_read(allowlist_sections, td->allowlist_count);
    }

    /* rewrites */
    int regex_count = 0;
    int erroff;
    const char *pcre_err;

    td->matcher_count = 0;
    mtev_conf_section_t *regexes = mtev_conf_get_sections_read(teams[i], "self::node()/path_rewrites/regex", &regex_count);
    if (regexes != NULL) {
      td->matcher = (pcre_matcher **)calloc(regex_count, sizeof(pcre_matcher *));

      for (int i = 0; i < regex_count; i++) {
        char *re = NULL;
        if (mtev_conf_get_string(regexes[i], "self::node()/@regex", &re) && re != NULL) {
          pcre *match = pcre_compile(re, 0, &pcre_err, &erroff, NULL);
          if(!match) {
            mtevL(tm_error, "pcre_compiled failed offset %d: %s\n", erroff, pcre_err);
            continue;
          }
          pcre_matcher *m = (pcre_matcher *)calloc(1, sizeof(pcre_matcher));
          m->match = match;
          m->extra = pcre_study(m->match, 0, &pcre_err);

          if (!mtev_conf_get_string(regexes[i], "self::node()/@replace", &m->replace)) {
            m->replace = strdup("{foo}");
          }
          td->matcher[td->matcher_count] = m;
          td->matcher_count++;
          free(re);
        }
      }
      mtev_conf_release_sections_read(regexes, regex_count);
    }
    tm_add_team(name, td);
    free(name);
  }
  mtev_conf_release_sections_read(teams, team_count);

  /* process jaeger thresholds */
  int threshold_count = 0;
  mtev_conf_section_t *thresholds = mtev_conf_get_sections_read(MTEV_CONF_ROOT, "/tm/service_thresholds/threshold", &threshold_count);
  for (int i = 0; i < threshold_count; i++) {
    char *name = NULL;
    char *threshold_ms = NULL;
    mtev_conf_get_string(thresholds[i], "@name", &name);
    mtev_conf_get_string(thresholds[i], "@threshold_ms", &threshold_ms);

    mtev_hash_replace(&threshold_hash, name, strlen(name), threshold_ms, free, free);
  }
  mtev_conf_release_sections_read(thresholds, threshold_count);

  mtev_conf_get_string(MTEV_CONF_ROOT, "/tm/infra_dest_url", &infra_dest_url);

  /*
   * trigger a periodic metric flush job
   *
   * This job reads through the metrics and expires and sends stuff that is flushable.
   *
   * This job will retrigger when it's done
   * to create a continuous loop
   */
  struct timeval tv;
  mtev_gettimeofday(&tv, NULL);
  tv.tv_sec += 10;
  eventer_add_at(metric_trigger_flush, get_all_team_data(), tv);

  /*
   * trigger a periodic transaction cleanup job
   *
   * This job reads through the transactions for a team an removes those that are over the transaction_lookback_threshold
   *
   * This job will retrigger when it's done
   * to create a continuous loop
   */
  tv.tv_sec += 10;
  eventer_add_at(trigger_transaction_cleanup, get_all_team_data(), tv);

  /*
   * trigger a periodic maintenance job
   *
   * This job calls out to a hook to get the jaeger thresholds per service
   * And triggers the send of the tracemate metrics to circonus
   *
   * This job will retrigger when it's done
   * to create a continuous loop
   */
  tv.tv_sec += 1;
  eventer_add_at(trigger_maintenance, NULL, tv);

  /*
   * trigger a periodic dashboard creation job
   *
   * This job reads through the services owned by this instance and checks circonus for the presence of 
   * a dashboard and related graphs, if missing, it will create one
   *
   * This job will retrigger when it's done
   * to create a continuous loop
   */
  if (_tm_create_visuals) {
    tv.tv_sec += 10;
    eventer_add_at(trigger_visuals, get_all_team_data(), tv);
  }
}

static ssize_t json_response_write(void *cl, const char *json, size_t s)
{
  mtev_http_session_ctx *resp = (mtev_http_session_ctx *)cl;
  mtev_http_response_append(resp, json, s);
  return s;
}

static int splat_stats(mtev_http_rest_closure_t *rc, int npats, char **pats)
{
  mtev_http_response_ok(rc->http_ctx, "application/json");
  stats_recorder_output_json(global_stats_recorder,
                             true, false, json_response_write,
                             rc->http_ctx);
  mtev_http_response_end(rc->http_ctx);
  return 0;
}

static int splat_stats_tagged(mtev_http_rest_closure_t *rc, int npats, char **pats)
{
  mtev_http_response_ok(rc->http_ctx, "application/json");
  stats_recorder_output_json_tagged(global_stats_recorder,
                             false, json_response_write,
                             rc->http_ctx);
  mtev_http_response_end(rc->http_ctx);
  return 0;
}

static int health_handler(mtev_http_rest_closure_t *rc, int npats, char **pats)
{
  mtev_http_response_ok(rc->http_ctx, "text/plain");
  mtev_http_response_append(rc->http_ctx, "OK\n", 3);
  mtev_http_response_end(rc->http_ctx);
  return 0;
}

static int version_handler(mtev_http_rest_closure_t *rc, int npats, char **pats)
{
  char version[256];
  tm_build_version(version, sizeof(version) - 1);
  mtev_http_response_ok(rc->http_ctx, "application/json");
  mtev_http_response_appendf(rc->http_ctx, "{\"version\": \"%s\"}", version);
  mtev_http_response_end(rc->http_ctx);
  return 0;
}

static int shutdown_service(mtev_http_rest_closure_t *rc, int npats, char **pats)
{
  mtev_http_response_ok(rc->http_ctx, "application/json");
  mtev_http_response_end(rc->http_ctx);
  exit(0);
  return 0;
}



static int child_main() {
  eventer_t e;
  char tm_version[80];

  /* Send out a birth notice. */
  mtev_watchdog_child_heartbeat();

  /* Load our config...
   * to ensure it is current w.r.t. to this child starting */
  if(mtev_conf_load(config_file) == -1) {
    mtevL(tm_error, "Cannot load config: '%s'\n", config_file);
    exit(2);
  }
  mtev_log_reopen_all();
  mtevL(tm_notice, "process starting: %d\n", (int)getpid());
  mtev_log_go_asynch();

  signal(SIGHUP, request_conf_reload);

  /* initialize the eventer */
  if(eventer_init() == -1) {
    mtevL(tm_stderr, "Cannot initialize eventer\n");
    exit(-1);
  }

  /* rotation init requires, eventer_init() */
  mtev_conf_log_init_rotate(APPNAME, mtev_false);

  /* Setup our heartbeat */
  mtev_watchdog_child_eventer_heartbeat();

  e = eventer_alloc_recurrent(notice_hup, NULL);
  eventer_add_recurrent(e);

  /* Initialize all of our listeners */
  mtev_dso_init();
  mtev_console_init(APPNAME);
  mtev_console_conf_init();
  mtev_capabilities_listener_init();
  tm_build_version(tm_version, sizeof(tm_version));
  mtev_http_rest_init();
  mtev_events_rest_init();
  mtev_stats_rest_init();
  mtev_heap_profiler_rest_init();
  mtev_listener_init(APPNAME);
  mtev_dso_post_init();

  /* Drop privileges */
  mtev_conf_security_init(APPNAME, droptouser, droptogroup, chrootpath);

  mtevAssert(mtev_http_rest_register_auth(
                                          "GET", "/stats/", "^(.*)$", splat_stats,
                                          mtev_http_rest_client_cert_auth
                                          ) == 0);
  mtevAssert(mtev_http_rest_register_auth(
                                          "GET", "/stats_tagged/", "^(.*)$", splat_stats_tagged,
                                          mtev_http_rest_client_cert_auth
                                          ) == 0);

  mtevAssert(mtev_http_rest_register_auth(
                                          "GET", "/", "^health.txt$", health_handler,
                                          mtev_http_rest_client_cert_auth
                                          ) == 0);

  mtevAssert(mtev_http_rest_register_auth(
                                          "GET", "/", "^version.json$", version_handler,
                                          mtev_http_rest_client_cert_auth
                                          ) == 0);

  /* Allow the tm web dashboard to be served (only if document_root is set) */
  mtevAssert(mtev_http_rest_register_auth(
                                          "GET", "/web/", "^(.*)$", mtev_rest_simple_file_handler,
                                          mtev_http_rest_client_cert_auth
                                          ) == 0);


  mtevAssert(mtev_http_rest_register_auth(
                                          "GET", "/", "^shutdown$", shutdown_service,
                                          mtev_http_rest_client_cert_auth
                                          ) == 0);

  tm_init_globals();
  tm_init();

  /* Write our log out, and setup a watchdog to write it out on change. */
  mtevL(mtev_debug, "tracemate ready\n");
  eventer_loop();
  return 0;
}

int main(int argc, char **argv) {
  parse_clargs(argc, argv);

  if (test) {
    tm_path_squasher_t *ps = tm_path_squasher_alloc(200);
    
    FILE *fp = fopen("url_list.txt", "r");
    if (fp == NULL)
      exit(EXIT_FAILURE);

    ssize_t read;
    size_t len = 0;
    char *line = NULL;
    while ((read = getline(&line, &len, fp)) != -1) {
      tm_path_squasher_add_path(ps, line);
    }

    mtev_hash_table *t = tm_path_squasher_get_regexes(ps);
    mtev_hash_iter it = MTEV_HASH_ITER_ZERO;
    while(mtev_hash_adv(t, &it)) {
      printf("{\"regex\": \"%s\",", it.key.str);
      printf("\"replace\": \"%s\"}\n", ((pcre_matcher *)it.value.ptr)->replace);
    }
    mtev_hash_destroy(t, free, pcre_matcher_destroy);
    tm_path_squasher_destroy(ps);

    fclose(fp);
    if (line) {
      free(line);
    }
  }

  curl_global_init(CURL_GLOBAL_ALL);

  int lock = MTEV_LOCK_OP_LOCK;
  mtev_memory_init();
  return mtev_main(APPNAME, config_file, debug, foreground,
                   lock, glider, droptouser, droptogroup,
                   child_main);
}
