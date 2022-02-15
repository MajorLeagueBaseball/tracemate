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

#ifndef TM_JAEGER_H
#define TM_JAEGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <mtev_json_object.h>
#include "tm_kafka.h"


void jaegerize_transaction(mtev_json_object *o, const char *jaeger_url, mtev_hash_table *span_errors);
void jaegerize_span(mtev_json_object *o, const char *jaeger_url, mtev_hash_table *span_errors);

#ifdef __cplusplus
}
#endif

#endif
