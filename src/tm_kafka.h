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

#ifndef TM_KAFKA_H
#define TM_KAFKA_H

#include "tm_utils.h"

#include <mtev_hooks.h>
#include <mtev_json_object.h>
#include <rdkafka.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct tm_kafka tm_kafka_t;
typedef struct tm_kafka_topic tm_kafka_topic_t;

/* creates tm_kafka_t * */
tm_kafka_t *tm_kafka_connect(const char *broker_list, rd_kafka_type_t type,  const char *group_id);

/* tm_kafka_t is no longer valid after this function returns */
void tm_kafka_disconnect(tm_kafka_t *t);

tm_kafka_topic_t *tm_kafka_start_consume(tm_kafka_t *t, const char *topic, int partition, int batch_size);
tm_kafka_topic_t *tm_kafka_produce_topic(tm_kafka_t *t, const char *topic);

int tm_kafka_get_partition(tm_kafka_topic_t *t);
const char *tm_kafka_get_name(tm_kafka_topic_t *t);
void tm_kafka_get_high_watermark(tm_kafka_topic_t *t, int64_t *watermark);

bool tm_kafka_produce(tm_kafka_topic_t *t, void *payload, size_t size, const void *key, size_t key_size);
bool tm_kafka_produce_on_partition(tm_kafka_topic_t *t, const void *payload, size_t size, int partition);

void tm_kafka_poll(tm_kafka_t *tm);

bool tm_kafka_stop_consume(tm_kafka_topic_t *t);

/* returns largest offset from the batch */
uint64_t tm_kafka_consume_messages(topic_stats_t *t);

#endif
