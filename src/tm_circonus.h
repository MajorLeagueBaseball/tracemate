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


#ifndef TM_CIRCONUS_H
#define TM_CIRCONUS_H

#include <mtev_dyn_buffer.h>

#include "tm_metric.h"
#include "tm_utils.h"

void send_infra_metrics(const char *url, stats_recorder_t *recorder);
bool journal_output_metrics(mtev_dyn_buffer_t *json, ssize_t count, const char *metric_submission_url);
bool circonus_curl_get(const char *api_key, mtev_dyn_buffer_t *response, const char *url, const char *search);
bool circonus_curl_post(const char *api_key, mtev_dyn_buffer_t *response, const char *url, const char *contents);

bool tm_circonus_init(const char *path);

#endif

