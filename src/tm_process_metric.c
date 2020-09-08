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
  char tag_string[3172];
  char agg_tag_string[2048];
  char team[128];
  uint64_t timestamp_ms;

  mtev_hash_table *team_metrics = pre_process(message, team, tag_string, &timestamp_ms);
  if (team_metrics == NULL) return false;

  if (!build_agg_tag_string(message, team, agg_tag_string)) {
    return false;
  }
  team_data_t *td = get_team_data(team);

  /* build aggregate metrics, this replaces host specific info with `all` */
  uint64_t agg_timestamp = ceil_timestamp(timestamp_ms);

  mtev_json_object *system = mtev_json_object_object_get(message, "system");
  mtev_json_object *jvm = mtev_json_object_object_get(message, "jvm");
  mtev_json_object *nodejs = mtev_json_object_object_get(message, "nodejs");
  mtev_json_object *golang = mtev_json_object_object_get(message, "golang");
  mtev_json_object *context = mtev_json_object_object_get(message, "context");
  mtev_json_object *service = mtev_json_object_object_get(context, "service");
  mtev_json_object *service_name = mtev_json_object_object_get(service, "name");
  mtev_json_object *agent = mtev_json_object_object_get(service, "agent");
  mtev_json_object *agent_name = mtev_json_object_object_get(agent, "name");

  const char *sn = mtev_json_object_get_string(service_name);
  const char *an = mtev_json_object_get_string(agent_name);

  service_info_t *sit = NULL;
  if (mtev_hash_retrieve(&td->service_data, sn, strlen(sn), (void **)&sit) && sit->service_agent == NULL) {
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

  char metric_name[4096];
  /* now build the various metric names and append the tag list */
  metric_key_t *key = NULL;
  metric_value_t *value = NULL;
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
          snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - latency", tag_string);
          uint64_t ts = ceil_timestamp(timestamp_ms);
          update_histogram(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(time), ts);
        }

        /* aggregate version */
        snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "jvm - memory - gc - latency", agg_tag_string);
        update_histogram(td, team_metrics, metric_name, true, mtev_json_object_get_double(time), agg_timestamp);


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
              snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - eventloop - latency", tag_string);
              uint64_t ts = ceil_timestamp(timestamp_ms);
              update_histogram(td, team_metrics, metric_name, true, mics, ts);
            }
            /* aggregate version */
            snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "nodejs - memory - eventloop - latency", agg_tag_string);
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
          uint64_t ts = ceil_timestamp(timestamp_ms);
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
        uint64_t ts = ceil_timestamp(timestamp_ms);
        update_average(td, team_metrics, metric_name, false, mtev_json_object_get_uint64(goroutines), ts);
      }
      /* aggregate version */
      snprintf(metric_name, sizeof(metric_name) - 1, "%s|ST[%s]", "golang - goroutines", agg_tag_string);
      update_average(td, team_metrics, metric_name, true, mtev_json_object_get_uint64(goroutines), agg_timestamp);
    }
  }

  return false;
}
