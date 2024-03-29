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

#include "tm_log.h"
#include "tm_metric.h"
#include "tm_process.h"
#include "tm_utils.h"

bool process_metric_message(topic_stats_t *stats, mtev_json_object *message)
{
  char metric_name[4096];
  char tag_string[3172];
  char agg_tag_string[2048];
  char team[128];
  uint64_t timestamp_ms;
  metric_key_t *key = NULL;
  metric_value_t *value = NULL;

  mtev_hash_table *team_metrics = pre_process(message, team, tag_string, &timestamp_ms);
  if (team_metrics == NULL) return false;

  if (!build_agg_tag_string(message, team, agg_tag_string)) {
    return false;
  }
  team_data_t *td = get_team_data(team);
  int mv = tm_apm_server_major_version(message);

  char *version = tm_apm_server_version(message);

  const char *service_string = tm_service_name(message);
  if (service_string == NULL) {
    mtevL(tm_error, "'metric' event is malformed, service name missing\n");
    stats_add64(stats->messages_errored, 1);
    return false;
  }

  uint64_t metric_flush_freq_ms = get_metric_flush_frequency_ms(service_string);
  /* build aggregate metrics, this replaces host specific info with `all` */
  uint64_t agg_timestamp = center_timestamp(timestamp_ms, metric_flush_freq_ms);

  if (version && strncmp(version, "7.15", strlen(version) > strlen("7.15") ? strlen("7.15") : strlen(version)) == 0) {
    
    /**
     * after 7.15 Elastic APM started to flatten the telemetry produced in it's `metric` documents.
     * They went from a nested structure:
     * 
     *   "nodejs": {
     *     "memory": {
     *        "heap": {
     *          "allocated": {
     *            "bytes": 3.7138432e+07
     *          },
     *          "used": {
     *            "bytes": 2.9087504e+07
     *          }
     *        }
     *      }
     *    },
     * 
     * To a flat representation:
     *   "nodejs.memory.external.bytes": 2.0242739e+07,
     */
    mtev_json_object *o = NULL;

    /** 
     * system
     */
    o = mtev_json_object_object_get(message, "system.memory.total");
    if (o) {
      mtev_json_object *total = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - total", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(total));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - total", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(total), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "system.memory.actual.free");
    if (o) {
      mtev_json_object *freemem = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - free", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(freemem));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - free", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(freemem), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "system.process.cpu.total.norm.pct");
    if (o) {
      mtev_json_object *pct = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - cpu - total - pct", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(pct));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - cpu - total - pct", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(pct), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "system.process.memory.size");
    if (o) {
      mtev_json_object *size = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - memory - size", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(size));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - memory - size", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(size), agg_timestamp);
    }


    o = mtev_json_object_object_get(message, "system.cpu.total.norm.pct");
    if (o) {
      mtev_json_object *pct = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - cpu - total - pct", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(pct));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - cpu - total - pct", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(pct), agg_timestamp);
    }

    /**
     * jvm
     */
    o = mtev_json_object_object_get(message, "jvm.thread.count");
    if (o) {
      mtev_json_object *count = o;
      if (count) {
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - thread - count", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_integer_value(mtev_json_object_get_uint64(count));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - thread - count", agg_tag_string);
        update_numeric(td, team_metrics, metric_name, true, (double)mtev_json_object_get_uint64(count), agg_timestamp);
      }
    }

    o = mtev_json_object_object_get(message, "jvm.memory.heap.committed");
    if (o) {
      mtev_json_object *committed = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - committed", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(committed));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - committed", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(committed), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "jvm.memory.heap.used");
    if (o) {
      mtev_json_object *used = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - used", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(used));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - used", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(used), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "jvm.memory.heap.max");
    if (o) {
      mtev_json_object *max = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - max", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(max));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - max", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(max), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "jvm.memory.non_heap.committed");
    if (o) {
      mtev_json_object *committed = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - committed", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(committed));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - committed", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(committed), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "jvm.memory.non_heap.used");
    if (o) {
      mtev_json_object *used = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - used", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(used));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - used", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(used), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "jvm.memory.non_heap.max");
    if (o) {
      mtev_json_object *max = o;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - max", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(max));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - max", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(max), agg_timestamp);
    }

    /* gc has extra info in the context object we are ignoring for now */
    mtev_json_object *time = mtev_json_object_object_get(message, "jvm.gc.time");
    mtev_json_object *count = mtev_json_object_object_get(message, "jvm.gc.count");
    mtev_json_object *alloc = mtev_json_object_object_get(message, "jvm.gc.alloc");
    if (time && count) {
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - total_latency", tag_string);
        uint64_t ts = center_timestamp(timestamp_ms, metric_flush_freq_ms);
        update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(time) / 1000.0, ts);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - total_latency", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(time) / 1000.0, agg_timestamp);

      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - count", tag_string);
        update_counter(td, team_metrics, metric_name, false, mtev_json_object_get_uint64(count), timestamp_ms);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - count", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(count), agg_timestamp);

    }
    if (alloc) {
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - alloc", tag_string);
        update_numeric(td, team_metrics, metric_name, false, mtev_json_object_get_double(alloc), timestamp_ms);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - alloc", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(alloc), agg_timestamp);
    }


    /**
     * nodejs
     */
    o = mtev_json_object_object_get(message, "nodejs.memory.heap.used.bytes");
    if (o) {
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - used", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(o));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - used", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(o), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "nodejs.memory.heap.allocated.bytes");
    if (o) {
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - allocated", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(o));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - allocated", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(o), agg_timestamp);
    }

    o = mtev_json_object_object_get(message, "nodejs.eventloop.delay.avg.ms");
    if (o) {
      double millis = mtev_json_object_get_double(o);
      double usecs = millis * 1000;
      uint64_t mics = (uint64_t)usecs;
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - eventloop - latency", tag_string);
        uint64_t ts = center_timestamp(timestamp_ms, metric_flush_freq_ms);
        update_histogram(td, team_metrics, metric_name, true, mics, ts);
      }
      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - eventloop - latency", agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, mics, agg_timestamp);
    }

    free(version);
    return false;
  }

  mtev_json_object *system = mtev_json_object_object_get(message, "system");
  mtev_json_object *jvm = mtev_json_object_object_get(message, "jvm");
  mtev_json_object *nodejs = mtev_json_object_object_get(message, "nodejs");
  mtev_json_object *golang = mtev_json_object_object_get(message, "golang");
  mtev_json_object *agent = NULL;
  if (mv > 6) {
    agent = mtev_json_object_object_get(message, "agent");
  } else {
    mtev_json_object *context = mtev_json_object_object_get(message, "context");
    if (context) {
      mtev_json_object *service = mtev_json_object_object_get(context, "service");
      if (service) {
        agent = mtev_json_object_object_get(service, "agent");
      }
    }
  }
  const char *an = NULL;
  if (agent != NULL) {
    mtev_json_object *agent_name = mtev_json_object_object_get(agent, "name");
    an = mtev_json_object_get_string(agent_name);
  }
  const char *sn = tm_service_name(message);

  service_info_t *sit = NULL;
  if (mtev_hash_retrieve(&td->service_data, sn, strlen(sn), (void **)&sit) && sit->service_agent == NULL && an != NULL) {
    sit->service_agent = strdup(an);
  }

  /* system metrics will resemble:
   *
   "system": {
     "memory": {
       "actual": {
         "free": 8.994816e+07
       },
       "total": 2.096164864e+09
     },
     "process": {
       "cpu": {
         "total": {
           "norm": {
             "pct": 0.01705197573372684
           }
         }
       },
       "memory": {
         "size": 3.163938816e+09
       }
     },
     "cpu": {
       "total": {
         "norm": {
           "pct": 0.25349249032596577
         }
       }
     }
   },
   *
   * JVM metrics will resemble:
   *
   "jvm": {
     "memory": {
       "heap": {
         "committed": 3.67525888e+08,
         "used": 1.77695632e+08,
         "max": 4.66092032e+08
       },
       "non_heap": {
         "max": -1,
         "committed": 1.48963328e+08,
         "used": 1.45037856e+08
       }
     },
     "gc": {
        "alloc": 9.29117408e+08
     }
   },

   * or
   *
   "jvm": {
     "gc": {
       "time": 1326,
       "count": 107
      }
   },
   "context": {
     "tags": {
       "name": "PS Scavenge"
     },
   }
   * 
   * nodejs metrics will resemble:
   * 
   *  "nodejs": {
        "memory": {
          "heap": {
            "used": {
              "bytes": 3.4495712e+07
             },
            "allocated": {
              "bytes": 3.948544e+07
            }
          }
        },
        "eventloop": {
          "delay": {
            "avg": {
              "ms": 0.15777981876693836
            },
            "ns": 0
          }
        },
        "requests": {
          "active": 3
        },
        "handles": {
          "active": 7
        }
      }
  * 
  * Golang metrics will resemble:
  * 
  *   "golang": {
         "heap": {
           "allocations": {
              "mallocs": 1.598348e+06,
              "objects": 21522,
              "frees": 1.576826e+06,
              "allocated": 7.134952e+06,
              "active": 9.4208e+06,
              "total": 2.58044352e+08,
              "idle": 5.6868864e+07
           },
           "gc": {
             "total_pause": {
                "ns": 2.0713076e+07
             },
             "cpu_fraction": 5.206868529274421e-06,
             "next_gc_limit": 1.3279504e+07,
             "total_count": 235
           },
           "system": {
             "total": 7.3482496e+07,
             "obtained": 6.6289664e+07,
             "released": 5.5730176e+07,
             "stack": 819200
           }
         },
         "goroutines": 11
      },
  * 
  * 
  * 
  */

  if (system) {
    mtev_json_object *mem = mtev_json_object_object_get(system, "memory");
    if (mem) {
      /* get actual.free and total */
      mtev_json_object *total = mtev_json_object_object_get(mem, "total");
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - total", tag_string);
        key = make_metric_key(metric_name, timestamp_ms);
        key->immediate_flush = true;
        value = make_numeric_value(mtev_json_object_get_double(total));
        mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
      }

      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - total", agg_tag_string);
      update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(total), agg_timestamp);

      mtev_json_object *actual = mtev_json_object_object_get(mem, "actual");
      if (actual) {
        mtev_json_object *freemem = mtev_json_object_object_get(actual, "free");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - free", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(freemem));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - memory - free", agg_tag_string);
        update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(freemem), agg_timestamp);
      }
    }
    mtev_json_object *process = mtev_json_object_object_get(system, "process");
    if (process) {
      mtev_json_object *cpu = mtev_json_object_object_get(process, "cpu");
      if (cpu) {
        mtev_json_object *total = mtev_json_object_object_get(cpu, "total");
        if (total) {
          mtev_json_object *norm = mtev_json_object_object_get(total, "norm");
          if (norm) {
            mtev_json_object *pct = mtev_json_object_object_get(norm, "pct");
            if (td->collect_host_level_system) {
              snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - cpu - total - pct", tag_string);
              key = make_metric_key(metric_name, timestamp_ms);
              key->immediate_flush = true;
              value = make_numeric_value(mtev_json_object_get_double(pct));
              mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
            }

            /* aggregate version */
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - cpu - total - pct", agg_tag_string);
            update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(pct), agg_timestamp);
          }
        }
      }
      mtev_json_object *mems = mtev_json_object_object_get(process, "memory");
      if (mems) {
        mtev_json_object *size = mtev_json_object_object_get(mems, "size");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - memory - size", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(size));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - process - memory - size", agg_tag_string);
        update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(size), agg_timestamp);
      }
    }
    mtev_json_object *cpu = mtev_json_object_object_get(system, "cpu");
    if (cpu) {
      mtev_json_object *total = mtev_json_object_object_get(cpu, "total");
      if (total) {
        mtev_json_object *norm = mtev_json_object_object_get(total, "norm");
        if (norm) {
          mtev_json_object *pct = mtev_json_object_object_get(norm, "pct");
          if (td->collect_host_level_system) {
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - cpu - total - pct", tag_string);
            key = make_metric_key(metric_name, timestamp_ms);
            key->immediate_flush = true;
            value = make_numeric_value(mtev_json_object_get_double(pct));
            mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
          }

          /* aggregate version */
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "system - cpu - total - pct", agg_tag_string);
          update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(pct), agg_timestamp);
        }
      }
    }
  }
  if (jvm) {
    mtev_json_object *mem = mtev_json_object_object_get(jvm, "memory");
    mtev_json_object *thread = mtev_json_object_object_get(jvm, "thread");
    if (thread) {
      mtev_json_object *count = mtev_json_object_object_get(thread, "count");
      if (count) {
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - thread - count", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_integer_value(mtev_json_object_get_uint64(count));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - thread - count", agg_tag_string);
        update_numeric(td, team_metrics, metric_name, true, (double)mtev_json_object_get_uint64(count), agg_timestamp);
      }
    }
    if (mem) {
      mtev_json_object *heap = mtev_json_object_object_get(mem, "heap");
      if (heap) {
        mtev_json_object *committed = mtev_json_object_object_get(heap, "committed");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - committed", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(committed));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - committed", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(committed), agg_timestamp);

        mtev_json_object *used = mtev_json_object_object_get(heap, "used");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - used", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(used));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - used", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(used), agg_timestamp);

        mtev_json_object *max = mtev_json_object_object_get(heap, "max");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - max", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(max));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - heap - max", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(max), agg_timestamp);

      }
      mtev_json_object *non_heap = mtev_json_object_object_get(mem, "non_heap");
      if (non_heap) {
        mtev_json_object *committed = mtev_json_object_object_get(non_heap, "committed");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - committed", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(committed));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - committed", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(committed), agg_timestamp);

        mtev_json_object *used = mtev_json_object_object_get(non_heap, "used");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - used", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(used));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - used", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(used), agg_timestamp);

        mtev_json_object *max = mtev_json_object_object_get(non_heap, "max");
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - max", tag_string);
          key = make_metric_key(metric_name, timestamp_ms);
          key->immediate_flush = true;
          value = make_numeric_value(mtev_json_object_get_double(max));
          mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - non_heap - max", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(max), agg_timestamp);

      }
    }
    mtev_json_object *gc = mtev_json_object_object_get(jvm, "gc");
    if (gc) {
      /* gc has extra info in the context object we are ignoring for now */
      mtev_json_object *time = mtev_json_object_object_get(gc, "time");
      mtev_json_object *count = mtev_json_object_object_get(gc, "count");
      mtev_json_object *alloc = mtev_json_object_object_get(gc, "alloc");
      if (time && count) {
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - total_latency", tag_string);
          uint64_t ts = center_timestamp(timestamp_ms, metric_flush_freq_ms);
          update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(time) / 1000.0, ts);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - total_latency", agg_tag_string);
        update_numeric(td, team_metrics, metric_name, true, mtev_json_object_get_double(time) / 1000.0, agg_timestamp);

        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - count", tag_string);
          update_counter(td, team_metrics, metric_name, false, mtev_json_object_get_uint64(count), timestamp_ms);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - count", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(count), agg_timestamp);

      }
      if (alloc) {
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - alloc", tag_string);
          update_numeric(td, team_metrics, metric_name, false, mtev_json_object_get_double(alloc), timestamp_ms);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - alloc", agg_tag_string);
        update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(alloc), agg_timestamp);
      }
    }
  }

  if (nodejs) {
    mtev_json_object *mem = mtev_json_object_object_get(nodejs, "memory");
    if (mem) {
      mtev_json_object *heap = mtev_json_object_object_get(mem, "heap");
      if (heap) {
        mtev_json_object *used = mtev_json_object_object_get(heap, "used");
        if (used) {
          mtev_json_object *bytes = mtev_json_object_object_get(used, "bytes");
          if (bytes) {
            if (td->collect_host_level_system) {
              snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - used", tag_string);
              key = make_metric_key(metric_name, timestamp_ms);
              key->immediate_flush = true;
              value = make_numeric_value(mtev_json_object_get_double(bytes));
              mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
            }

            /* aggregate version */
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - used", agg_tag_string);
            update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(bytes), agg_timestamp);
          }
        }

        mtev_json_object *allocated = mtev_json_object_object_get(heap, "allocated");
        if (allocated) {
          mtev_json_object *bytes = mtev_json_object_object_get(allocated, "bytes");
          if (bytes) {
            if (td->collect_host_level_system) {
              snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - allocated", tag_string);
              key = make_metric_key(metric_name, timestamp_ms);
              key->immediate_flush = true;
              value = make_numeric_value(mtev_json_object_get_double(bytes));
              mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
            }

            /* aggregate version */
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - heap - allocated", agg_tag_string);
            update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(bytes), agg_timestamp);
          }
        }
      }
    }

    mtev_json_object *eventloop = mtev_json_object_object_get(nodejs, "eventloop");
    if (eventloop) {
      /* gc has extra info in the context object we are ignoring for now */
      mtev_json_object *delay = mtev_json_object_object_get(eventloop, "delay");
      if (delay) {
        mtev_json_object *avg = mtev_json_object_object_get(delay, "avg");
        if (avg) {
          mtev_json_object *ms = mtev_json_object_object_get(avg, "ms");
          if (ms) {
            double millis = mtev_json_object_get_double(ms);
            double usecs = millis * 1000;
            uint64_t mics = (uint64_t)usecs;
            if (td->collect_host_level_system) {
              snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - eventloop - latency", tag_string);
              uint64_t ts = center_timestamp(timestamp_ms, metric_flush_freq_ms);
              update_histogram(td, team_metrics, metric_name, true, mics, ts);
            }
            /* aggregate version */
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - eventloop - latency", agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, mics, agg_timestamp);
          }
        }
      }
    }
  }

  if (golang) {
    mtev_json_object *heap = mtev_json_object_object_get(golang, "heap");
    if (heap) {
      mtev_json_object *allocations = mtev_json_object_object_get(heap, "allocations");
      if (allocations) {
        /* in golang, the metric: 'allocated' means how much of the heap is currently used. */
        mtev_json_object *active = mtev_json_object_object_get(allocations, "allocated");
        if (active) {
          if (td->collect_host_level_system) {
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - memory - heap - used", tag_string);
            key = make_metric_key(metric_name, timestamp_ms);
            key->immediate_flush = true;
            value = make_numeric_value(mtev_json_object_get_double(active));
            mtev_hash_replace(team_metrics, (const char *)key, key->key_len, value, metric_key_free, metric_value_free);
          }

          /* aggregate version */
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - memory - heap - used", agg_tag_string);
          update_average(td, team_metrics, metric_name, true, mtev_json_object_get_double(active), agg_timestamp);
        }
      }
    }

    mtev_json_object *gc = mtev_json_object_object_get(heap, "gc");
    if (gc) {
      /* gc has extra info in the context object we are ignoring for now */
      mtev_json_object *total_pause = mtev_json_object_object_get(gc, "total_pause");
      mtev_json_object *count = mtev_json_object_object_get(gc, "total_count");
      mtev_json_object *total_pause_ns = mtev_json_object_object_get(total_pause, "ns");
      if (total_pause_ns && count) {
        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - memory - gc - total_pause_ns", tag_string);
          uint64_t ts = center_timestamp(timestamp_ms, metric_flush_freq_ms);
          update_counter(td, team_metrics, metric_name, false, mtev_json_object_get_double(total_pause_ns), ts);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - memory - gc - total_pause_ns", agg_tag_string);
        update_counter(td, team_metrics, metric_name, true, mtev_json_object_get_double(total_pause_ns), agg_timestamp);


        if (td->collect_host_level_system) {
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - memory - gc - total_count", tag_string);
          update_counter(td, team_metrics, metric_name, false, mtev_json_object_get_uint64(count), timestamp_ms);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - memory - gc - total_count", agg_tag_string);
        update_counter(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(count), agg_timestamp);

      }
    }

    mtev_json_object *goroutines = mtev_json_object_object_get(golang, "goroutines");
    if (goroutines) {
      if (td->collect_host_level_system) {
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - goroutines", tag_string);
        uint64_t ts = center_timestamp(timestamp_ms, metric_flush_freq_ms);
        update_average(td, team_metrics, metric_name, false, mtev_json_object_get_uint64(goroutines), ts);
      }
      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - goroutines", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(goroutines), agg_timestamp);
    }
  }

  return false;
}
