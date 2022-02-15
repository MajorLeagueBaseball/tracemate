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

extern "C" {
#include <stdint.h>
#include <mtev_json_object.h>
#include <mtev_log.h>
#include "tm_process.h"
#include "tm_utils.h"
#include "tm_metric.h"
}

#include <iostream>
#include <sstream>
#include "tm_process_otel_span.h"

#include "proto/opentelemetry/proto/collector/trace/v1/trace_service.pb.h"
#include "proto/jaeger.pb.h"

/* 
 * These are considered valid tag values in Circonus and won't require base64 encoding to save.
 * 
   perl -e '$valid = qr/[`+A-Za-z0-9!@#\$%^&"'\/\?\._:=-]/;
   foreach $i (0..7) {
   foreach $j (0..31) { printf "%d,", chr($i*32+$j) =~ $valid; }
   print "\n";
 * */
static uint8_t vtagmap_value[256] = 
{
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,1,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,0,1,
 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,
 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};


static const opentelemetry::proto::common::v1::KeyValue*
get_attribute(const ::opentelemetry::proto::trace::v1::Span &span, const char *name)
{
  for (int i = 0; i < span.attributes_size(); i++) {
    const opentelemetry::proto::common::v1::KeyValue &kv = span.attributes(i);
    if (kv.key() == name) {
      return &kv;
    }
  }
  return NULL;
}

static const proto::KeyValue*
get_tag(const ::proto::Span &span, const char *name)
{
  for (int i = 0; i < span.tags_size(); i++) {
    const proto::KeyValue &kv = span.tags(i);
    if (kv.key() == name) {
      return &kv;
    }
  }
  return NULL;
}

/**
 * replace tag unfriendly chars with underscores,
 */
static const char *
make_safe(std::string &s) {
  /* yes, I intend to fuck with internal string memory */
  char *x = (char *)s.c_str();
  for (size_t i = 0; i < s.size(); i++) {
    if (vtagmap_value[(uint8_t)x[i]] == 0) {
      x[i] = '_';
    }
  }
  return (const char *)x;
}

static char *
parse_domain(const std::string &url_s) {
  std::string domain;
  const std::string prot_end("://");
  std::string::const_iterator prot_i = std::search(url_s.begin(), url_s.end(),
                                         prot_end.begin(), prot_end.end());
  if( prot_i == url_s.end() )
    return NULL;
  std::advance(prot_i, prot_end.length());
  std::string::const_iterator path_i = std::find(prot_i, url_s.end(), '/');
  domain.reserve(std::distance(prot_i, path_i));
  std::transform(prot_i, path_i,
            back_inserter(domain),
                 std::ptr_fun<int,int>(std::tolower)); // host is icase

  return strdup(domain.c_str());
}

static char *
simplify_status_code(int64_t code) {
  int64_t x = code / 100;
  char *rval = NULL;
  asprintf(&rval, "%dXX", (int)x);
  return rval;
}

extern "C"
bool otel_process_jaeger_span(rd_kafka_message_t *m, topic_stats_t *ts)
{
  char metric_name[4096];
  char tag_string[3172];
  char agg_tag_string[2048];

  /**
   * The inbound data from OTel collector arrives as Jaeger Span objects.
   */
  proto::Span s;

  if (!s.ParseFromArray(m->payload, m->len)) {
    mtevL(mtev_error, "Failed to parse OTel jaeger span of size: %d\n", m->len);
    return false;
  }

  if (!s.has_process()) {
    mtevL(mtev_error, "Illegal jaeger span, missing process! %s:%s\n", s.trace_id().c_str(), s.span_id().c_str());
    return false;
  }

  const proto::Process &p = s.process();

  const char* service = p.service_name().c_str();
  const char* team = NULL;
  for (int j = 0; j < p.tags_size(); j++) {
    const proto::KeyValue& kv = p.tags(j);
    if (kv.key() == "mlb.team") {
      team = kv.v_str().c_str();
    }
  }
  if (service == NULL) {
    mtevL(mtev_error, "Illegal OTel span, no service.name declared\n");
    return false;
  }

  std::cout << "Service: " << service << std::endl;
  team_data_t *td = NULL;
  if (team == NULL) {
    mtevL(mtev_debug, "OTel span missing mlb.team attribute, lumping into universal APM account\n");
    team = "unknown";
    td = get_team_data("unknown");
  } else {
    std::cout << "Team:" << team << std::endl;
    td = get_team_data(team);
  }

  if (!td) {
    mtevL(tm_debug, "Team not configured: %s\n", team);
    return false;
  }
  mtev_hash_table *team_metrics = &td->hash;
  uint64_t metric_flush_freq_ms = get_metric_flush_frequency_ms(service);

  /**
   * these tags will be on all produced metrics.
   *
   * For now, we want to use __rollup:false to indicate to circonus that
   * we don't want this data to stick around until the tagging methodology for otel
   * is stabilized
   */
  snprintf(agg_tag_string, sizeof(agg_tag_string) - 1, "service:%s,team:%s,__rollup:false", service, team);

  /**
   * Calculate the duration of the span in microseconds
   */
  uint64_t start = ((uint64_t)s.start_time().seconds() * 1000000) + s.start_time().nanos() / 1000;
  uint64_t duration_us = (s.duration().seconds() * 1000000) + (s.duration().nanos() / 1000);
  uint64_t end = start + duration_us;

  /**
   * Metrics should be recorded at the end of the span, not the start
   */
  uint64_t timestamp = end / 1000;

  /**
   * We want to center the aggregation inside the minute it represents for purposes of aggregration
   * this is the most honest place for a metric to reside.
   *
   * When this metric is eventually flushed to the downstream telemetry system, tracemate
   * will add jitter to prevent any future aggregations at the same timestamp
   * from stepping on older aggregations at the same place.
   */
  uint64_t agg_timestamp = center_timestamp(timestamp, metric_flush_freq_ms);

  std::cout << "Span duration: " << duration_us << "us" << std::endl;

  /**
   * Span processing relies on well documented convention for span attributes in Otel.
   *
   * https://github.com/open-telemetry/opentelemetry-specification/tree/main/specification/trace/semantic_conventions
   *
   * These attribute naming conventions give us the roadmap to synthesizing metrics from OTel spans as
   * well as the roadmap to conversion of Elastic APM documents to OTel spans.
   */
  const std::string& trace_id = s.trace_id();
  const std::string& span_id = s.span_id();

  /* certain high cardinality metrics can contain a special tag that prevents them from being rolled up
   * for longer term storage
   */
  // const char *rollup = "";
  // if (td->rollup_high_cardinality == mtev_false) {
  //   rollup = ",__rollup:false";
  // }

  if (get_tag(s, "span.kind")->v_str() == "server") {

    /**
     * an OTel Span with kind SERVER is a top level span and processed like the top level transaction in elastic APM world
     */
    std::cout << "Spankind == SERVER, top level span" << std::endl;
    const proto::KeyValue* method = get_tag(s, "http.method");
    const proto::KeyValue* flavor = get_tag(s, "http.flavor");
    const proto::KeyValue* status_code = get_tag(s, "http.status_code");
    const char *flav = "unknown";
    const char *method_s = method->v_str().c_str();
    int64_t sc_int = 0;
    char *code = NULL;

    if (flavor != NULL) {
      flav = flavor->v_str().c_str();
    }
    if (status_code != NULL) {
      sc_int = status_code->v_int64();
      code = simplify_status_code(sc_int);
    }

    /* average latency for this service and transaction name */
    std::string op_name = s.operation_name();
    const char *safe_op_name = make_safe(op_name);
    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:server,name:%s,method:%s,code:%s,flavor:%s,%s]",
             safe_op_name, method_s, code, flav, agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* average latency for the service in total */
    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:server,name:all,method:%s,code:%s,flavor:%s,%s]",
             method_s, code, flav, agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* request count for the service and span name */
    snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:server,name:%s,method:%s,code:%s,flavor:%s,%s]",
             safe_op_name, method_s, code, flav, agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    /* request count for the service in total */
    snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:server,name:all,method:%s,code:%s,flavor:%s,%s]",
             method_s, code, flav, agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    if (sc_int > 399) {
      snprintf(metric_name, sizeof(metric_name) - 1, "error_count|ST[type:server,code:%s,name:%s,method:%s,flavor:%s,%s]",
               code, safe_op_name, method_s, flav, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
      snprintf(metric_name, sizeof(metric_name) - 1, "error_count|ST[type:server,code:%s,name:all,method:%s,flavor:%s,%s]",
               code, method_s, flav, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
    }
    free(code);
  } else if (get_tag(s, "span.kind")->v_str() == "client") {
    std::cout << "Spankind == CLIENT, dependent span" << std::endl;

    /**
     * - If it has an attribute "db.system" it is a database span
     * - If it has an attribute "http.url" it is an external span
     * -
     *
     */
    const proto::KeyValue* attr = NULL;
    if ((attr = get_tag(s, "db.system")) != NULL) {
      const std::string& subtype = attr->v_str();
      // copy on purpose
      std::string op_name = s.operation_name();
      const char *safe_op_name = make_safe(op_name);

      snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:db,system:%s,op_name:%s,%s]",
               subtype.c_str(), safe_op_name, agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:db,system:%s,op_name:all,%s]",
               subtype.c_str(), agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:db,system:%s,op_name:%s,%s]",
               subtype.c_str(), safe_op_name, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:db,system:%s,op_name:all,%s]",
               subtype.c_str(), agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    } else if ((attr = get_tag(s, "http.url")) != NULL) {
      const proto::KeyValue* method = get_tag(s, "http.method");
      const proto::KeyValue* code_kv = get_tag(s, "http.status_code");
      char *domain = parse_domain(attr->v_str().c_str());
      char *code = simplify_status_code(code_kv->v_int64());

      /* aggregate for external */
      snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:http,method:%s,domain:%s,code:%s,%s]",
               method->v_str().c_str(), domain, code, agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      /* aggregate for service */
      snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:http,method:%s,domain:all,code:%s,%s]",
               method->v_str().c_str(), code, agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:http,method:%s,domain:%s,code:%s,%s]",
               method->v_str().c_str(), domain, code, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:http,method:%s,domain:all,code:%s,%s]",
               method->v_str().c_str(), code, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      free(domain);
      free(code);
    } else if ((attr = get_tag(s, "rpc.system")) != NULL) {
      const std::string& system = attr->v_str();
      std::string op_name = s.operation_name();
      const char *safe_op_name = make_safe(op_name);

      /* aggregate for external */
      snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:rpc,system:%s,method:%s,%s]",
               system.c_str(), safe_op_name, agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      /* aggregate for service */
      snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:rpc,system:%s,method:all,%s]",
               system.c_str(), agg_tag_string);
      update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:rpc,system:%s,method:%s,%s]",
               system.c_str(), safe_op_name, agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

      snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:rpc,system:%s,method:all,%s]",
               system.c_str(), agg_tag_string);
      update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    }
  } else if (get_tag(s, "span.kind")->v_str() == "producer") {
    const std::string& system = get_tag(s, "messaging.system")->v_str();
    const std::string& dest = get_tag(s, "messaging.destination")->v_str();

    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:producer,system:%s,destination:%s,%s]",
             system.c_str(), dest.c_str(), agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* aggregate for service */
    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:producer,system:%s,destination:all,%s]",
             system.c_str(), agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:producer,system:%s,destination:%s,%s]",
             system.c_str(), dest.c_str(), agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:producer,system:%s,destination:all,%s]",
             system.c_str(), agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
  } else if (get_tag(s, "span.kind")->v_str() == "consumer") {
    const std::string& system = get_tag(s, "messaging.system")->v_str();
    const std::string& dest = get_tag(s, "messaging.destination")->v_str();

    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:consumer,system:%s,destination:%s,%s]",
             system.c_str(), dest.c_str(), agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* aggregate for service */
    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:consumer,system:%s,destination:all,%s]",
             system.c_str(), agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:consumer,system:%s,destination:%s,%s]",
             system.c_str(), dest.c_str(), agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:consumer,system:%s,destination:all,%s]",
             system.c_str(), agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
  } else if (get_tag(s, "span.kind")->v_str() == "internal") {
    std::string op_name = s.operation_name();
    const char *safe_op_name = make_safe(op_name);

    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:internal,op_name:%s,%s]",
             safe_op_name, agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    /* aggregate for service */
    snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:internal,op_name:all,%s]",
             agg_tag_string);
    update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "execution_count|ST[type:internal,op_name:%s,%s]",
             safe_op_name, agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

    snprintf(metric_name, sizeof(metric_name) - 1, "execution_count|ST[type:internal,op_name:all,%s]",
             agg_tag_string);
    update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
  }
}


extern "C"
bool otel_process_otel_span(rd_kafka_message_t *m, topic_stats_t *ts)
{
  char metric_name[4096];
  char tag_string[3172];
  char agg_tag_string[2048];

  /**
   * The inbound data from OTel collector is meant for sharing with
   * other OTel collectors, so it may be batched into several spans
   * even though there is a single kafka message.
   *
   * It arrives as an ExportTraceServiceRequest
   */
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest s;

  if (!s.ParseFromArray(m->payload, m->len)) {
    mtevL(mtev_error, "Failed to parse OTel span of size: %d\n", m->len);
    return false;
  }

  mtevL(mtev_debug, "Success parse of OTel span of size: %d\n", m->len);
  for (int i = 0; i < s.resource_spans_size(); i++) {
    const ::opentelemetry::proto::trace::v1::ResourceSpans& span = s.resource_spans(i);
    /**
     * the Resource is required to have a "service.name" key with a string value
     * that identifies the service.  It is also required to have a "mlb.team" key
     * with a string value that indicates the related engineering team abbreviation that
     * owns the service.
     */
    const ::opentelemetry::proto::resource::v1::Resource& r = span.resource();
    const char* service = NULL;
    const char* team = NULL;
    for (int j = 0; j < r.attributes_size(); j++) {
      const ::opentelemetry::proto::common::v1::KeyValue& kv = r.attributes(j);
      if (kv.key() == "service.name") {
        service = kv.value().string_value().c_str();
      }
      else if (kv.key() == "mlb.team") {
        team = kv.value().string_value().c_str();
      }
    }
    if (service == NULL) {
      mtevL(mtev_error, "Illegal OTel span, no service.name declared\n");
      return false;
    }
    std::cout << "Service: " << service << std::endl;
    team_data_t *td = NULL;
    if (team == NULL) {
      mtevL(mtev_debug, "OTel span missing mlb.team attribute, lumping into universal APM account\n");
      team = "unknown";
      td = get_team_data("unknown");
    } else {
      std::cout << "Team:" << team << std::endl;
      td = get_team_data(team);
    }

    if (!td) {
      mtevL(tm_debug, "Team not configured: %s\n", team);
      return false;
    }
    mtev_hash_table *team_metrics = &td->hash;
    uint64_t metric_flush_freq_ms = get_metric_flush_frequency_ms(service);

    /**
     * these tags will be on all produced metrics.
     *
     * For now, we want to use __rollup:false to indicate to circonus that
     * we don't want this data to stick around until the tagging methodology for otel
     * is stabilized
     */
    snprintf(agg_tag_string, sizeof(agg_tag_string) - 1, "service:%s,team:%s,__rollup:false", service, team);

    /**
     * the InstrumentationLibrarySpans tells us which otel library produced the
     * spans as well as being a container for the spans themselves.
     */
    for (int j = 0; j < span.instrumentation_library_spans_size(); j++) {
      const ::opentelemetry::proto::trace::v1::InstrumentationLibrarySpans& ils = span.instrumentation_library_spans(j);
      std::cout << "InstrumentationLibrary: " << ils.instrumentation_library().name()
                << ", version: " << ils.instrumentation_library().version() << std::endl;
      for (int k = 0; k < ils.spans_size(); k++) {
        const ::opentelemetry::proto::trace::v1::Span &real_span = ils.spans(k);
        std::cout << real_span.name() << ", state:" << real_span.trace_state() << std::endl;

        /**
         * Calculate the duration of the span in microseconds
         */
        uint64_t start = real_span.start_time_unix_nano();
        uint64_t end = real_span.end_time_unix_nano();

        uint64_t duration_us = end > start ? (end - start)/1000 : 0;
        /**
         * Metrics should be recorded at the end of the span, not the start
         */
        uint64_t timestamp = end / 1000000;

        /**
         * We want to center the aggregation inside the minute it represents for purposes of aggregration
         * this is the most honest place for a metric to reside.
         *
         * When this metric is eventually flushed to the downstream telemetry system, tracemate
         * will add jitter to prevent any future aggregations at the same timestamp
         * from stepping on older aggregations at the same place.
         */
        uint64_t agg_timestamp = center_timestamp(timestamp, metric_flush_freq_ms);

        std::cout << "Span duration: " << duration_us << "us" << std::endl;

        /**
         * Span processing relies on well documented convention for span attributes in Otel.
         *
         * https://github.com/open-telemetry/opentelemetry-specification/tree/main/specification/trace/semantic_conventions
         *
         * These attribute naming conventions give us the roadmap to synthesizing metrics from OTel spans as
         * well as the roadmap to conversion of Elastic APM documents to OTel spans.
         */
        const std::string& trace_id = real_span.trace_id();
        const std::string& span_id = real_span.span_id();

        /* certain high cardinality metrics can contain a special tag that prevents them from being rolled up
         * for longer term storage
         */
        const char *rollup = "";
        if (td->rollup_high_cardinality == mtev_false) {
          rollup = ",__rollup:false";
        }

        if (real_span.kind() == opentelemetry::proto::trace::v1::Span_SpanKind_SPAN_KIND_SERVER) {

          /**
           * an OTel Span with kind SERVER is a top level span and processed like the top level transaction in elastic APM world
           */
          std::cout << "Spankind == SERVER, top level span" << std::endl;
          const opentelemetry::proto::common::v1::KeyValue* method = get_attribute(real_span, "http.method");
          const opentelemetry::proto::common::v1::KeyValue* flavor = get_attribute(real_span, "http.flavor");
          const opentelemetry::proto::common::v1::KeyValue* status_code = get_attribute(real_span, "http.status_code");
          const char *flav = "unknown";
          const char *method_s = method->value().string_value().c_str();
          char sc[10] = {"unknown"};
          int sc_int = 0;

          if (flavor != NULL) {
            flav = flavor->value().string_value().c_str();
          }
          if (status_code != NULL) {
            sc_int = status_code->value().int_value();
            sprintf(sc, "%d", sc_int);
          }

          /* average latency for this service and transaction name */
          const std::string& span_name = real_span.name();
          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:server,name:%s,method:%s,flavor:%s,%s]",
                   span_name.c_str(), method_s, flav, agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          /* average latency for the service in total */
          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:server,name:all,method:%s,flavor:%s,%s]",
                   method_s, flav, agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          /* request count for the service and span name */
          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:server,name:%s,method:%s,flavor:%s,%s]",
                   span_name.c_str(), method_s, flav, agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

          /* request count for the service in total */
          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:server,name:all,method:%s,flavor:%s,%s]",
                   method_s, flav, agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

          if (sc_int > 399) {
            snprintf(metric_name, sizeof(metric_name) - 1, "error_count|ST[type:server,code:%s,name:%s,method:%s,flavor:%s,%s]",
                     sc, span_name.c_str(), method_s, flav, agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
            snprintf(metric_name, sizeof(metric_name) - 1, "error_count|ST[type:server,code:%s,name:all,method:%s,flavor:%s,%s]",
                     sc, method_s, flav, agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
          }
        } else if (real_span.kind() == opentelemetry::proto::trace::v1::Span_SpanKind_SPAN_KIND_CLIENT) {

          /**
           * - If it has an attribute "db.system" it is a database span
           * - If it has an attribute "http.url" it is an external span
           * -
           *
           */
          const opentelemetry::proto::common::v1::KeyValue* attr = NULL;
          if ((attr = get_attribute(real_span, "db.system")) != NULL) {
            const std::string& subtype = attr->value().string_value();

            snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:db,system:%s,name:%s,%s]",
                     subtype.c_str(), real_span.name().c_str(), agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:db,system:%s,name:all,%s]",
                     subtype.c_str(), agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:db,system:%s,name:%s,%s]",
                     subtype.c_str(), real_span.name().c_str(), agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:db,system:%s,name:all,%s]",
                     subtype.c_str(), agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
          } else if ((attr = get_attribute(real_span, "http.url")) != NULL) {

            /* aggregate for external */
            snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:http,name:%s,%s]",
                     real_span.name().c_str(), agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

            /* aggregate for service */
            snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:http,name:all,%s]",
                     agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:http,name:%s,%s]",
                     real_span.name().c_str(), agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:http,name:all,%s]",
                     agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
          } else if ((attr = get_attribute(real_span, "rpc.system")) != NULL) {
            const std::string& system = attr->value().string_value();

            /* aggregate for external */
            snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:rpc,system:%s,name:%s,%s]",
                     system.c_str(), real_span.name().c_str(), agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

            /* aggregate for service */
            snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:client,subtype:rpc,system:%s,name:all,%s]",
                     system.c_str(), agg_tag_string);
            update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:rpc,system:%s,name:%s,%s]",
                     system.c_str(), real_span.name().c_str(), agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:client,subtype:rpc,system:%s,name:all,%s]",
                     system.c_str(), agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

          }
        } else if (real_span.kind() == opentelemetry::proto::trace::v1::Span_SpanKind_SPAN_KIND_PRODUCER) {
          const std::string& system = get_attribute(real_span, "messaging.system")->value().string_value();
          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:producer,system:%s,name:%s,%s]",
                   system.c_str(), real_span.name().c_str(), agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          /* aggregate for service */
          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:producer,system:%s,name:all,%s]",
                   system.c_str(), agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:producer,system:%s,name:%s,%s]",
                   system.c_str(), real_span.name().c_str(), agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:producer,system:%s,name:all,%s]",
                   system.c_str(), agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
        } else if (real_span.kind() == opentelemetry::proto::trace::v1::Span_SpanKind_SPAN_KIND_CONSUMER) {
          const std::string& system = get_attribute(real_span, "messaging.system")->value().string_value();

          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:consumer,system:%s,name:%s,%s]",
                   system.c_str(), real_span.name().c_str(), agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          /* aggregate for service */
          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:consumer,system:%s,name:all,%s]",
                   system.c_str(), agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:consumer,system:%s,name:%s,%s]",
                   system.c_str(), real_span.name().c_str(), agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:consumer,system:%s,name:all,%s]",
                   system.c_str(), agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
        } else if (real_span.kind() == opentelemetry::proto::trace::v1::Span_SpanKind_SPAN_KIND_INTERNAL) {

          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:internal,name:%s,%s]",
                   real_span.name().c_str(), agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          /* aggregate for service */
          snprintf(metric_name, sizeof(metric_name) - 1, "latency|ST[type:internal,name:all,%s]",
                   agg_tag_string);
          update_histogram(td, team_metrics, metric_name, true, duration_us, agg_timestamp);

          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:internal,name:%s,%s]",
                   real_span.name().c_str(), agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

          snprintf(metric_name, sizeof(metric_name) - 1, "request_count|ST[type:internal,name:all,%s]",
                   agg_tag_string);
          update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
        }

        /**
         * deal with exceptions
         */
        for (int e = 0; e < real_span.events_size(); e++) {
          const opentelemetry::proto::trace::v1::Span_Event& event = real_span.events(e);
          /**
           * https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/semantic_conventions/exceptions.md
           *
           * "The name of the event MUST be "exception"
           */
          if (event.name() == "exception") {
            snprintf(metric_name, sizeof(metric_name) - 1, "exception_count|ST[name:%s,%s]",
                     real_span.name().c_str(), agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);

            snprintf(metric_name, sizeof(metric_name) - 1, "exception_count|ST[name:all,%s]",
                     agg_tag_string);
            update_counter(td, team_metrics, metric_name, true, 1, agg_timestamp);
          }
        }
      }
    }
  }
}
