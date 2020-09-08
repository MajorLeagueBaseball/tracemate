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

#include "tm_hooks.h"
#include "tm_kafka.h"
#include "tm_log.h"
#include "tm_process.h"
#include "tm_utils.h"

#include <mtev_memory.h>

struct tm_kafka {
  rd_kafka_t *rk;
  char *brokers;
};

struct tm_kafka_topic {
  rd_kafka_topic_t *rkt;
  tm_kafka_t *t;
  char *topic;
  int partition;
  int batch_size;
};

static void tm_kafka_logger (const rd_kafka_t *rk, int level,
                             const char *fac, const char *buf)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	fprintf(stderr, "%u.%03u RDKAFKA-%i-%s: %s: %s\n",
          (int)tv.tv_sec, (int)(tv.tv_usec / 1000),
          level, fac, rk ? rd_kafka_name(rk) : NULL, buf);
}

static void 
msg_delivered(rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, void *opaque)
{
  if (rkmessage->err) {
    mtevL(tm_error, "%% Message delivery failed: %s [%"PRId32"]: %s\n",
          rd_kafka_topic_name(rkmessage->rkt),
          rkmessage->partition,
          rd_kafka_err2str(rkmessage->err));
  } else {
    mtevL(tm_debug, "%% Message delivery succeeded: %s [%"PRId32"]\n",
          rd_kafka_topic_name(rkmessage->rkt),
          rkmessage->partition);
  }
}


tm_kafka_t *tm_kafka_connect(const char *broker_list, rd_kafka_type_t type, const char *group_id)
{
  char errstr[512];
  char tmp[16];

  tm_kafka_t *t = (tm_kafka_t *)malloc(sizeof(tm_kafka_t));
  t->brokers = strdup(broker_list);

  rd_kafka_conf_t *conf = rd_kafka_conf_new();

  rd_kafka_conf_set_log_cb(conf, tm_kafka_logger);
  rd_kafka_conf_set_dr_msg_cb(conf, msg_delivered);

  snprintf(tmp, sizeof(tmp), "%i", SIGIO);
	rd_kafka_conf_set(conf, "internal.termination.signal", tmp, NULL, 0);
  rd_kafka_conf_set(conf, "enable.auto.commit", "true",
                    NULL, 0);
  rd_kafka_conf_set(conf, "auto.commit.interval.ms", "1000",
                    NULL, 0);
  rd_kafka_conf_set(conf, "enable.auto.offset.store", "true",
                    NULL, 0);
  rd_kafka_conf_set(conf, "queue.buffering.max.ms", "1000",
                    NULL, 0);
  rd_kafka_conf_set(conf, "queue.buffering.max.kbytes", "10000",
                    NULL, 0);
  rd_kafka_conf_set(conf, "request.required.acks", "1",
                    NULL, 0);
  rd_kafka_conf_set(conf, "produce.offset.report", "true",
                    NULL, 0);
  rd_kafka_conf_set(conf, "compression.codec", "lz4",
                    NULL, 0);
  rd_kafka_conf_set(conf, "queued.min.messages", "10000", NULL, 0);
  //rd_kafka_conf_set(conf, "fetch.message.max.bytes", "25000", NULL, 0);
  rd_kafka_conf_set(conf, "queued.max.message.kbytes", "1000000", NULL, 0);

  /* rd_kafka_conf_set(conf, "debug", "broker,topic,msg", */
  /*                   NULL, 0);  */
  rd_kafka_conf_set(conf, "statistics.interval.ms", "2000",
                    NULL, 0);
  rd_kafka_conf_set(conf, "group.id", group_id,
                    NULL, 0);

  /* Create Kafka handle */
  if (!(t->rk = rd_kafka_new(type, conf,
                             errstr, sizeof(errstr)))) {
    mtevFatal(tm_error,
            "%% Failed to create new consumer: %s\n",
            errstr);
  }

  /* Add brokers */
  if (rd_kafka_brokers_add(t->rk, t->brokers) == 0) {
    mtevFatal(tm_error, "%% No valid brokers specified\n");
  }

  return t;
}

void tm_kafka_disconnect(tm_kafka_t *t)
{
  // tear down
  rd_kafka_destroy(t->rk);
  free(t->brokers);
  free(t);
}

void
tm_kafka_poll(tm_kafka_t *t)
{
  rd_kafka_poll(t->rk, 5);
}

tm_kafka_topic_t *tm_kafka_start_consume(tm_kafka_t *t, const char *topic, int partition, int batch_size, int64_t offset)
{
  tm_kafka_topic_t *top = (tm_kafka_topic_t *)malloc(sizeof(tm_kafka_topic_t));
  top->topic = strdup(topic);

  /* Topic configuration */
	rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();

  /* Create topic */
  top->rkt = rd_kafka_topic_new(t->rk, top->topic, topic_conf);
  top->t = t;
  topic_conf = NULL; /* Now owned by topic */

  top->partition = partition;
  top->batch_size = batch_size;

  /* Start consuming */
  if (rd_kafka_consume_start(top->rkt, top->partition, offset) == -1) {
    rd_kafka_resp_err_t err = rd_kafka_last_error();
    mtevL(tm_error, "%% Failed to start consuming: %s\n",
            rd_kafka_err2str(err));
    if (err == RD_KAFKA_RESP_ERR__INVALID_ARG)
      mtevL(tm_error,
              "%% Broker based offset storage "
              "requires a group.id, "
              "add: -X group.id=yourGroup\n");
    free(top->topic);
    rd_kafka_topic_destroy(top->rkt);
    free(top);
    return NULL;
  }
  return top;
}

tm_kafka_topic_t *tm_kafka_produce_topic(tm_kafka_t *t, const char *topic)
{
  tm_kafka_topic_t *top = (tm_kafka_topic_t *)malloc(sizeof(tm_kafka_topic_t));
  top->topic = strdup(topic);

  /* Topic configuration */
	rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();

  /* Create topic */
  top->rkt = rd_kafka_topic_new(t->rk, top->topic, topic_conf);
  top->t = t;
  topic_conf = NULL; /* Now owned by topic */

  return top;
}

bool tm_kafka_produce(tm_kafka_topic_t *t, void *payload, size_t size, const void *key, size_t key_size)
{
/* #ifdef NO_PUBLISH */
/*   return true; */
/* #else */
 retry:
  if (rd_kafka_produce(t->rkt, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY, payload, size,
                       key, key_size, NULL) == -1) {
    rd_kafka_resp_err_t err = rd_kafka_last_error();
    if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
      tm_kafka_poll(t->t);
      goto retry;
    }
    mtevL(tm_error, "%% Failed to produce: %s\n",
          rd_kafka_err2str(err));
    return false;
  }

  return true;
/* #endif */
}

bool tm_kafka_produce_on_partition(tm_kafka_topic_t *t, const void *payload, size_t size, int partition)
{
#ifdef NO_PUBLISH
  return true;
#else
 retry:
  if (rd_kafka_produce(t->rkt, partition, RD_KAFKA_MSG_F_COPY, (void *)payload, size,
                       NULL, 0, NULL) == -1) {
    rd_kafka_resp_err_t err = rd_kafka_last_error();
    if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
      tm_kafka_poll(t->t);
      goto retry;
    }
    mtevL(tm_error, "%% Failed to produce: %s\n",
          rd_kafka_err2str(err));
    return false;
  }
  return true;
#endif
}


int tm_kafka_get_partition(tm_kafka_topic_t *t)
{
  return t->partition;
}

const char *tm_kafka_get_name(tm_kafka_topic_t *t)
{
  return t->topic;
}

void tm_kafka_get_high_watermark(tm_kafka_topic_t *t, int64_t *watermark)
{
  int64_t low = 0;
  rd_kafka_get_watermark_offsets(t->t->rk, t->topic, t->partition, &low, watermark);
}

bool tm_kafka_stop_consume(tm_kafka_topic_t *t)
{
  int r = rd_kafka_consume_stop(t->rkt, (int32_t)t->partition);
  if (r == -1) {
    mtevL(tm_error,
            "%% Error in consume_stop: %s\n",
            rd_kafka_err2str(rd_kafka_last_error()));
    return false;
  }
  return true;
}

static bool
tm_kafka_handle_message(topic_stats_t *stats, const char *event, mtev_json_object *top, int ttl)
{
  bool keep = false;
  if (strcmp(event, "metric") == 0) {
    keep = process_metric_message(stats, top);
  } else if (strcmp(event, "transaction") == 0) {
    keep = process_transaction_message(stats, top, ttl);
  } else if (strcmp(event, "span") == 0) {
    keep = process_span_message(stats, top, ttl);
  } else if (strcmp(event, "error") == 0) {
    keep = process_error_message(stats, top, ttl);
  } else if (strcmp(event, "aggregate") == 0) {
    keep = process_aggregate_message(stats, top);
  } else if (strcmp(event, "url") == 0) {
    keep = process_url_message(stats, top);
  } else if (strcmp(event, "regex") == 0) {
    keep = process_regex_message(stats, top);
  } else {
    mtevL(tm_error, "Unknown processor.event '%s'\n", event);
    stats_add64(stats->messages_errored, 1);
  }
  return keep;
}

static bool
tm_kafka_process_message(rd_kafka_message_t *m, topic_stats_t *stats)
{
  /*
   * The incoming message is a JSON formatted elastic APM messages
   *
   * The field "processor.event" could be one of many types:
   *
   * metric - system or runtime level metric
   * transaction - the root span of a distributed trace
   * span - a member span of a distributed trace
   * error - error condition
   * aggregate - special synthetic type not produced by elastic APM for aggregation
   *
   * Every incoming message will have a context.service.name that identifies where the data came from.
   * The string will resemble: {teamname}-{servicename}
   *
   * The {teamname} portion of this will map to the <teams> section in the config file to determine the settings for that
   * team.
   *
   * Each of these types is handled slightly differently:
   *
   * metric      - named and tagged appropriately and saved in a hashmap for the owning team where the key is the timestamp
   *               of the event and the value is a double
   * transaction - used to create histogram data if the transaction is longer than the defined config for histograms and also
   *               used to determine if this transaction should be sent to Jaeger based on the jaeger threshold.
   * span        - used to create histogram data if the transaction is longer than the defined config for histograms..
   *               these are hung onto until we determine if the owning transaction needs to be sent to jaeger
   * error       - only used to determine if the parent transaction needs to be sent to jaeger
   *
   */
  if (m != NULL && m->err == 0) {
    stats_add64(stats->messages_seen, 1);
    struct mtev_json_tokener *tokener = mtev_json_tokener_new();
    const char *json = m->payload;
    if (json != NULL) {
      mtev_json_object *top = mtev_json_tokener_parse_ex(tokener, json, m->len);
      mtev_json_tokener_free(tokener);

      if (top) {
        uint64_t start_us = mtev_now_us();
        ck_pr_store_64(&stats->kafka_offset, m->offset);
        stats_add64(stats->messages_processed, 1);

        /* every message has a "processor" object and a "context" object */
        mtev_json_object *processor = mtev_json_object_object_get(top, "processor");
        mtev_json_object *context = mtev_json_object_object_get(top, "context");
        if (!processor || !context) {
          mtevL(tm_error, "Invalid elastic APM data\n");
          stats_add64(stats->messages_errored, 1);
          mtev_json_object_put(top);
          return true;
        }
        const char *event = mtev_json_object_get_string(mtev_json_object_object_get(processor, "event"));

        /* 
         * connected devices will be sending specialized transactions through a proxy which
         * we need to pre-process these here and turn them into valid elastic APM
         * JSON documents to be processed normally by the ensuing blocks .
         * 
         * Rather than pollute this rather generic code with MLB specific processing,
         * I am externalizing this specialized processing into a module
         */
        mtev_json_object *result = NULL;
        int ttl = 600;
        int rc = elastic_apm_document_hook_invoke(top, &result, &ttl);

        bool keep = false;
        if (rc == MTEV_HOOK_ABORT) {
          stats_add64(stats->messages_filtered, 1);
          keep = false;
          goto skip;
        }

        keep = tm_kafka_handle_message(stats, event, top, ttl);

      skip:
        if (!keep) {
          mtev_json_object_put(top);
        }

        if (result != NULL) {
          /**
           * the hook is allowed to produce another document to replace or add to the incoming document, process result
           */
          mtev_json_object *result_processor = mtev_json_object_object_get(result, "processor");
          mtev_json_object *result_context = mtev_json_object_object_get(result, "context");
          if (result_processor && result_context) {
            const char *result_event = mtev_json_object_get_string(mtev_json_object_object_get(result_processor, "event"));
            bool k = tm_kafka_handle_message(stats, result_event, result, ttl);
            if (k == false) {
              mtev_json_object_put(result);
            }
          }
        }

        uint64_t end_us = mtev_now_us();
        stats_set_hist_intscale(stats->message_process_latency, end_us - start_us, -6, 1);
        return true;
      }
    }
  } else {
    int empty = strcmp("Broker: No more messages", (char *)m->payload);
    if (empty != 0) {
      stats_add64(stats->messages_errored, 1);
      mtevL(tm_error, "Error consuming kafka message: %s\n", (char *)m->payload);
    }
  }
  return false;
}

uint64_t 
tm_kafka_consume_messages(topic_stats_t *ts)
{
  uint64_t offset = 0;
  rd_kafka_message_t **rkmessages = (rd_kafka_message_t **)calloc(sizeof(rd_kafka_message_t *), ts->topic->batch_size);
  ssize_t r = rd_kafka_consume_batch(ts->topic->rkt, ts->topic->partition,
                                     100,
                                     rkmessages,
                                     ts->topic->batch_size);
  if (r != -1) {
    for (int i = 0 ; i < r ; i++) {
      if (tm_kafka_process_message(rkmessages[i], ts) && offset < rkmessages[i]->offset) {
        offset = rkmessages[i]->offset;
      }
      rd_kafka_message_destroy(rkmessages[i]);
    }
  }
  free(rkmessages);

  return offset;
}


