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

#include "tm_circonus.h"
#include "tm_utils.h"

#include <curl/curl.h>
#include <jlog.h>
#include <mtev_b64.h>
#include <mtev_dyn_buffer.h>
#include <mtev_thread.h>

struct send_data {
  const char *json;
  size_t len;
  size_t sent;
};

static jlog_ctx *jlog;
static pthread_mutex_t jlog_write_mutex;
static const char *journal_path;
static pthread_t sender_thread;

static size_t curl_output(void *contents, size_t size, size_t nmemb, void *closure)
{
  mtev_dyn_buffer_t *buffer = (mtev_dyn_buffer_t *)closure;
  if (buffer) {
    mtev_dyn_buffer_add(buffer, (uint8_t *)contents, size * nmemb);
  }
  return size * nmemb;
}

static size_t send_metric_input(char *buffer, size_t size, size_t nitems, void *closure)
{
  struct send_data *sd = (struct send_data *)closure;
  if (sd) {
    size_t used = sd->len;
    size_t remaining = used - sd->sent;
    mtevL(tm_debug, "Remaining to send: %zu\n", remaining);
    if (remaining == 0) {
      mtevL(tm_debug, "No more remaining end upload\n");
      return 0;
    }
    size_t send = MIN(size * nitems, remaining);
    memcpy(buffer, sd->json + sd->sent, send);
    sd->sent += send;
    return send;
  }
  return CURL_READFUNC_ABORT;
}

void *
send_thread(void *arg)
{
  mtev_thread_setname("circonus_send");
  /* setup reader */
  jlog_ctx *jlog_reader = jlog_new(journal_path);
  int rc = jlog_ctx_open_reader(jlog_reader, "reader");
  if (rc != 0 && jlog_ctx_err(jlog_reader) == JLOG_ERR_INVALID_SUBSCRIBER) {
    jlog_ctx_close(jlog_reader);
    jlog_reader = jlog_new(journal_path);
    jlog_ctx_add_subscriber(jlog_reader, "reader", JLOG_BEGIN);
    rc = jlog_ctx_open_reader(jlog_reader, "reader");
    if (rc != 0) {
      mtevFatal(tm_error, "Cannot open jlog reader: %s\n", jlog_ctx_err_string(jlog_reader));
    }
  }

  while(true) {
    jlog_id begin, end;
    char begins[20];
    int msg_count = 0;
    jlog_message message;
    if((msg_count = jlog_ctx_read_interval(jlog_reader, &begin, &end)) == -1) {
      mtevL(tm_error, "jlog_ctx_read_interval failed: %d %s\n", jlog_ctx_err(jlog_reader), jlog_ctx_err_string(jlog_reader));
      usleep(1000);
      continue;
    }
    if(msg_count > 0) {
      for(int i = 0; i < msg_count; i++, JLOG_ID_ADVANCE(&begin)) {
        end = begin;
        if(jlog_ctx_read_message(jlog_reader, &begin, &message) != 0) {
          mtevL(tm_error, "read failed: %d\n", jlog_ctx_err(jlog_reader));
        } else {
          jlog_snprint_logid(begins, sizeof(begins), &begin);
          mtevL(tm_error, "Processing logid: %s\n", begins);
          mtev_json_tokener *tok = mtev_json_tokener_new();
          mtev_json_object *wrapper = mtev_json_tokener_parse_ex(tok, (const char *)message.mess, message.mess_len);
          bool do_checkpoint = false;
          mtev_json_tokener_free(tok);
          if (wrapper) {
            mtev_json_object *url = mtev_json_object_object_get(wrapper, "url");
            mtev_json_object *o = mtev_json_object_object_get(wrapper, "data");
            if (url == NULL || o == NULL) {
              mtevL(tm_error, "Bad wrapper object in journal, missing submission url or data object, skipping\n");
              do_checkpoint = true;
              goto checkpoint;
            }
            const char *metric_submission_url = mtev_json_object_get_string(url);
            /* the data object is a base64 encoded string we have to turn back into a regular string */
            const char *b64 = mtev_json_object_get_string(o);
            size_t b64_len = strlen(b64);
            size_t decode_len = mtev_b64_max_decode_len(b64_len);
            char *json = (char *)malloc(decode_len);
            size_t len = mtev_b64_decode(b64, b64_len, (unsigned char *)json, decode_len);
#ifdef NO_PUBLISH

            tok = mtev_json_tokener_new();
            o = mtev_json_tokener_parse_ex(tok, (const char *)json, len);
            mtev_json_tokener_free(tok);

            size_t c = 0;
            uint64_t now = mtev_now_ms();
            uint64_t total_skew = 0;
            do_checkpoint = true;
            mtevL(mtev_debug, "Sending to url: %s\n", metric_submission_url);
            mtev_json_object_object_foreach(o, key, m) {
              c++;
              if (m) {
                mtev_json_object *ts = mtev_json_object_object_get(m, "_ts");
                if (ts) {
                  uint64_t mts = mtev_json_object_get_uint64(ts);
                  mtevL(tm_debug, "Now: %" PRIu64 ", TS: %" PRIu64 ", Skew: %" PRIu64 "ms, key: %s\n", now, mts, now - mts, key);
                  total_skew += (now - mts);
                }
              }
            }
            if (c > 0) {
              mtevL(mtev_error, "Avg skew: %" PRIu64 "\n", (total_skew/c));
            }
            mtev_json_object_put(o);
#endif
            char errors[CURL_ERROR_SIZE];
            mtev_dyn_buffer_t response_buffer;
            mtev_dyn_buffer_t header_buffer;

            mtev_dyn_buffer_init(&response_buffer);
            mtev_dyn_buffer_init(&header_buffer);

            CURLM *curl_multi = curl_multi_init();
            CURL *curl = curl_easy_init();

            struct curl_slist *list = NULL;
            //struct curl_slist *resolve_list = NULL;
            struct send_data sd =
              {
               .json = json,
               .len = len,
               .sent = 0
              };

            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_URL, metric_submission_url);
            curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 131072L);
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, send_metric_input);
            curl_easy_setopt(curl, CURLOPT_READDATA, &sd);
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_output);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errors);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(curl, CURLOPT_EXPECT_100_TIMEOUT_MS, 500L);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_output);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_buffer);

            list = curl_slist_append(list, "Content-Type: application/json");
            // chunked encoding is wonky, leave it out for now
            //list = curl_slist_append(list, "Transfer-Encoding: chunked");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

            curl_multi_add_handle(curl_multi, curl);

#ifndef NO_PUBLISH
            int repeats = 0;
            int still_running = 0;

            do {
              CURLMcode mc = CURLM_OK;
              int numfds = 0;
              mc = curl_multi_perform(curl_multi, &still_running);

              if(mc == CURLM_OK) {
                /* wait for activity, timeout or "nothing" */
                //mtevL(tm_error, "curl_multi wait code %d.n", mc);
                mc = curl_multi_wait(curl_multi, NULL, 0, 1, &numfds);
              }

              if(mc != CURLM_OK) {
                mtevL(tm_error, "curl_multi failed, code %d.n", mc);
                break;
              }

              /* 'numfds' being zero means either a timeout or no file descriptors to
                 wait for. Try timeout on first occurrence, then assume no file
                 descriptors and no file descriptors to wait for means wait for 10
                 milliseconds. */
              if(!numfds) {
                repeats++; /* count number of repeated zero numfds */
                if(repeats > 1) {
                  usleep(10000); /* sleep 10 milliseconds */
                }
                if (repeats >= 1000) {
                  do_checkpoint = false;
                  goto after_response;
                }
              }
              else
                repeats = 0;
            } while(still_running);


            struct CURLMsg *m = NULL;
            do {
              int msgq = 0;
              m = curl_multi_info_read(curl_multi, &msgq);
              if(m && (m->msg == CURLMSG_DONE)) {
                CURL *e = m->easy_handle;
                CURLcode res = m->data.result;
                if(res != CURLE_OK) {
                  mtevL(tm_error, "curl_easy_perform(%s) failed: %s, %s\n\n%.*s\n\n%.*s\n",
                        metric_submission_url, curl_easy_strerror(res), errors, 
                        (int)mtev_dyn_buffer_used(&header_buffer), mtev_dyn_buffer_data(&header_buffer),
                        (int)mtev_dyn_buffer_used(&response_buffer), mtev_dyn_buffer_data(&response_buffer));
                } else {
                  do_checkpoint = true;
                  mtevL(tm_error, "Response: %.*s\n", (int)mtev_dyn_buffer_used(&response_buffer), mtev_dyn_buffer_data(&response_buffer));
                  if (mtev_dyn_buffer_used(&response_buffer) > 0) {
                    /* parse the response and compare it to what we expected 
                     * 
                     * In addition, look for "filtered" count and track metric so we can alert
                     */
                    mtev_json_tokener *tok = mtev_json_tokener_new();
                    mtev_json_object *resp = mtev_json_tokener_parse_ex(tok, 
                                                                        (const char *)mtev_dyn_buffer_data(&response_buffer), 
                                                                        mtev_dyn_buffer_used(&response_buffer));
                    mtev_json_tokener_free(tok);
                    if (resp != NULL) {
                      mtev_json_object *stats = mtev_json_object_object_get(resp, "stats");
                      uint64_t s = mtev_json_object_get_uint64(stats);
                      if (s != -1) {
                        mtevL(tm_error, "Trap reported: %" PRIu64 "\n", s);
                      }
                      mtev_json_object *filtered = mtev_json_object_object_get(resp, "filtered");
                      uint64_t f = mtev_json_object_get_uint64(filtered);
                      if (f != 0) {
                        mtevL(tm_error, "%" PRIu64 " metrics were filtered\n", f);
                      }
                      mtev_json_object_put(resp);
                    }
                  }
                }
                curl_multi_remove_handle(curl_multi, e);
                curl_easy_cleanup(e);
              }
            } while(m);
          after_response:
#else
            curl_multi_remove_handle(curl_multi, curl);
            curl_easy_cleanup(curl);
            do_checkpoint = true;
#endif
            mtev_dyn_buffer_destroy(&response_buffer);
            mtev_dyn_buffer_destroy(&header_buffer);

            curl_slist_free_all(list);
            curl_multi_cleanup(curl_multi);
            free(json);
          }
        checkpoint:
          mtev_json_object_put(wrapper);
          if (do_checkpoint) {
            if(jlog_ctx_read_checkpoint(jlog_reader, &end) != 0) {
              mtevL(tm_error, "checkpoint failed: %d %s\n", jlog_ctx_err(jlog_reader), jlog_ctx_err_string(jlog_reader));
            } else {
              mtevL(tm_error, "\tcheckpointed...\n");
            }
          }
        }
      }
    } else {
      usleep(1000);
    }
  }
  return NULL;
}

bool
tm_circonus_init(const char *path)
{
  pthread_mutex_init(&jlog_write_mutex, NULL);
  int rc = 0;
  jlog = jlog_new(path);
  if ((rc = jlog_ctx_init(jlog)) != 0) {
    if (jlog_ctx_err(jlog) != JLOG_ERR_CREATE_EXISTS) {
      mtevL(tm_error, "Cannot init jlog: %s\n", jlog_ctx_err_string(jlog));
      return false;
    } else {
      jlog_ctx_close(jlog);
      jlog = jlog_new(path);
    }
  } else {
    jlog_ctx_close(jlog);
    jlog = jlog_new(path);
  }
  jlog_ctx_set_multi_process(jlog, 0);
  if (jlog_ctx_open_writer(jlog) != 0) {
    mtevL(tm_error, "Cannot open jlog writer: %s\n", jlog_ctx_err_string(jlog));
    return false;
  }

  journal_path = strdup(path);
  /* spawn the sender thread */
  if (pthread_create(&sender_thread, NULL, send_thread, NULL) != 0) {
    return false;
  }
  return true;
}

bool
journal_output_metrics(mtev_dyn_buffer_t *json, ssize_t count, const char *metric_submission_url)
{
  if (jlog == NULL) return false;
  if (mtev_dyn_buffer_used(json) < 3 || count == 0) return true;

  /* we're gonna base64 the data payload, the json in the payload is generated by hand
   * and we don't want it to traverse the mtev_json parse path as that may alter it subtly
   */
  size_t b64_size = mtev_b64_encode_len(mtev_dyn_buffer_used(json));

  mtev_dyn_buffer_t b64;
  mtev_dyn_buffer_init(&b64);
  mtev_dyn_buffer_ensure(&b64, b64_size + 10);
  size_t used = mtev_b64_encode(mtev_dyn_buffer_data(json), mtev_dyn_buffer_used(json),
                                (char *)mtev_dyn_buffer_write_pointer(&b64), mtev_dyn_buffer_size(&b64));
  mtev_dyn_buffer_advance(&b64, used);

  size_t url_len = strlen(metric_submission_url);
  mtev_dyn_buffer_t message;
  mtev_dyn_buffer_init(&message);
  mtev_dyn_buffer_ensure(&message, url_len + used + 32);

  mtev_dyn_buffer_add(&message, (uint8_t *)"{\"url\":\"", 8);
  mtev_dyn_buffer_add(&message, (uint8_t *)metric_submission_url, url_len);
  mtev_dyn_buffer_add(&message, (uint8_t *)"\",\"data\":\"", 10);
  mtev_dyn_buffer_add(&message, mtev_dyn_buffer_data(&b64), used);
  mtev_dyn_buffer_add(&message, (uint8_t *)"\"}", 2);

  pthread_mutex_lock(&jlog_write_mutex);
  if (jlog_ctx_write(jlog, mtev_dyn_buffer_data(&message), mtev_dyn_buffer_used(&message)) != 0) {
    pthread_mutex_unlock(&jlog_write_mutex);
    mtevL(tm_error, "Cannot write to journal: %s\n", jlog_ctx_err_string(jlog));
    mtev_dyn_buffer_destroy(&message);
    return false;
  }
  pthread_mutex_unlock(&jlog_write_mutex);
  mtev_dyn_buffer_destroy(&message);
  mtev_dyn_buffer_destroy(&b64);
  return true;
}


static ssize_t infra_stats_write(void *cl, const char *json, size_t s)
{
  mtev_dyn_buffer_t *json_buffer = (mtev_dyn_buffer_t *)cl;
  mtev_dyn_buffer_add(json_buffer, (uint8_t *)json, s);
  return s;
}

void
send_infra_metrics(const char *infra_dest_url, stats_recorder_t *global_stats_recorder)
{
  if (infra_dest_url == NULL || global_stats_recorder == NULL) return;

  mtev_dyn_buffer_t buffer;
  mtev_dyn_buffer_init(&buffer);
  stats_recorder_output_json_tagged(global_stats_recorder,
                                    true, infra_stats_write,
                                    &buffer);

  journal_output_metrics(&buffer, -1, infra_dest_url);

  mtev_dyn_buffer_destroy(&buffer);
}

bool
circonus_curl_get(const char *api_key, mtev_dyn_buffer_t *response, const char *url, const char *search)
{
  char resolved_url[4096];
  char errors[CURL_ERROR_SIZE];
  char header[256];

  snprintf(resolved_url, sizeof(resolved_url), url, search);

  CURL *curl = curl_easy_init();

  struct curl_slist *list = NULL;
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_URL, resolved_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_output);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errors);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  snprintf(header, sizeof(header), "X-Circonus-Auth-Token: %s", api_key);
  list = curl_slist_append(list, header);
  list = curl_slist_append(list, "X-Circonus-App-Name: tracemate");
  list = curl_slist_append(list, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  curl_slist_free_all(list);
  return res == CURLE_OK;
}


bool
circonus_curl_post(const char *api_key, mtev_dyn_buffer_t *response, const char *url, const char *contents)
{
  char header[256];
  char errors[CURL_ERROR_SIZE];
  CURL *curl = curl_easy_init();

  struct curl_slist *list = NULL;
  size_t len = strlen(contents);
  struct send_data sd =
    {
     .json = contents,
     .len = len,
     .sent = 0
    };

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, len);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, send_metric_input);
  curl_easy_setopt(curl, CURLOPT_READDATA, &sd);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_output);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errors);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  snprintf(header, sizeof(header), "X-Circonus-Auth-Token: %s", api_key);
  list = curl_slist_append(list, "Accept: application/json");
  list = curl_slist_append(list, "User-Agent: tracemate");
  list = curl_slist_append(list, header);
  list = curl_slist_append(list, "X-Circonus-App-Name: tracemate");
  list = curl_slist_append(list, "Content-Type: application/json");
  list = curl_slist_append(list, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  curl_slist_free_all(list);
  return res == CURLE_OK;
}
