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

#include "tm_jaeger.h"

extern "C" {
#include "tm_utils.h"
#include "tm_process.h"
#include <curl/curl.h>
#include <pthread.h>
}

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>


#include <ostream>
#include <sstream>
#include <map>
#include "proto/jaeger.pb.h"
#include "proto/collector.pb.h"
#include "proto/collector.grpc.pb.h"

#define TYP_INIT 0
#define TYP_SMLE 1
#define TYP_BIGE 2

static uint64_t
hex2uint64BE(const char *hex, size_t len) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "0x%.*s", len, hex);

  errno = 0;
  char *end;
  uint64_t x = strtoull(buffer, &end, 16);
  return htonll(x);
}

struct trace_id {
  uint64_t low;
  uint64_t high;
};

static void
fill_trace_id(const char *hex, struct trace_id *tid) {
  tid->high = 0;
  tid->low = 0;
  if (hex == NULL) return;

  size_t l = strlen(hex);
  if (l > 32) { return; }

  if (l == 32) {
    /* decoding on other side expects BIGE */
    tid->low = hex2uint64BE(hex, 16);
    tid->high = hex2uint64BE(hex + 16, 16);
  } else if (l == 16) {
    tid->low = hex2uint64BE(hex, 16);
  }
}

static char *
build_stack_trace(mtev_json_object *stack_trace)
{
  if (stack_trace == NULL || !mtev_json_object_is_type(stack_trace, mtev_json_type_array)) {
    return strdup("");
  }

  /**
   * 
   * A java stack resembles:
   * 
   "stacktrace": [
   {
   "abs_path": "com.mysql.jdbc.StatementImpl",
   "function": "execute",
   "library_frame": true,
   "exclude_from_grouping": false,
   "line": {
   "number": 745
   },
   "filename": "StatementImpl.java"
   },
   {
   "abs_path": "org.apache.tomcat.jdbc.pool.PooledConnection",
   "function": "validate",
   "library_frame": true,
   "exclude_from_grouping": false,
   "line": {
   "number": 532
   },
   "filename": "PooledConnection.java"
   },
   {
   "filename": "PooledConnection.java",
   "abs_path": "org.apache.tomcat.jdbc.pool.PooledConnection",
   "function": "validate",
   "library_frame": true,
   "exclude_from_grouping": false,
   "line": {
   "number": 443
   }
   },
   * 
   * Newer apm agent use "module" instead of "abs_path"
  */
  std::ostringstream trace;
  /* never more than 20 frames */
  int frame_count = mtev_json_object_array_length(stack_trace);
  int len = MIN(frame_count, 20);
  for (int i = 0; i < len; i++) {
    mtev_json_object *s = mtev_json_object_array_get_idx(stack_trace, i);
    mtev_json_object *path = mtev_json_object_object_get(s, "abs_path");
    if (path == NULL) {
      path = mtev_json_object_object_get(s, "module");
    }
    trace << mtev_json_object_get_string(path)
          << "."
          << mtev_json_object_get_string(mtev_json_object_object_get(s, "function"))
          << "("
          << mtev_json_object_get_string(mtev_json_object_object_get(s, "filename"))
          << ":"
          << mtev_json_object_get_int(mtev_json_object_object_get(mtev_json_object_object_get(s, "line"), "number"))
          << ")"
          << "\n";
  }
  if (frame_count > 20) {
    trace << "...\n";
  }
  return strdup(trace.str().c_str());
}

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

class JaegerClient {
public:
  JaegerClient(std::shared_ptr<Channel> channel)
    : stub_(jaeger::api_v2::CollectorService::NewStub(channel)) {}
  JaegerClient(JaegerClient &&other) : stub_(std::move(other.stub_)) {}

  bool send(const jaeger::api_v2::PostSpansRequest &batch) const {
#ifndef NO_PUBLISH
    ClientContext context;
    jaeger::api_v2::PostSpansResponse response;
    Status status = stub_->PostSpans(&context, batch, &response);
    if (!status.ok()) {
      mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
      mtevL(debug_jaeger, "Failure to send to jaeger: %s\n", status.error_message().c_str());
    }
    return status.ok();
#else
    return true;
#endif
  }
private:
  std::unique_ptr<jaeger::api_v2::CollectorService::Stub> stub_;
};

static std::map<std::string, JaegerClient> jaeger_connections;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

const JaegerClient&
get_jaeger_client(const char *jaeger_url)
{
  mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
  mtevL(debug_jaeger, "Making connection to jaeger_url: %s\n", jaeger_url);
  
  pthread_mutex_lock(&mutex);
  auto c = jaeger_connections.find(jaeger_url);
  if (c == jaeger_connections.end()) {
    jaeger_connections.emplace(
      std::make_pair(std::string(jaeger_url), 
                     JaegerClient(
                       grpc::CreateChannel(jaeger_url, 
                                           grpc::InsecureChannelCredentials())) 
                     )
                 );

    c = jaeger_connections.find(jaeger_url);
    if (c == jaeger_connections.end()) {
      mtevFatal(tm_error, "Can't find thing I just inserted?!\n");
    }
    pthread_mutex_unlock(&mutex);
    return c->second;
  }
  pthread_mutex_unlock(&mutex);
  return c->second;
}

#define RETURN_IF_MISSING(result,o,field)                  \
  mtev_json_object *result = NULL;                         \
  do {                                                     \
    if (o == NULL) return;                                 \
    result = mtev_json_object_object_get(o, field);        \
    if (result == NULL) return;                            \
  } while(false)

extern "C" void jaegerize_transaction(mtev_json_object *message, const char *jaeger_url)
{
  const JaegerClient &jaeger_client = get_jaeger_client(jaeger_url);

  mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
  mtevL(debug_jaeger, "TRANS: %s\n", mtev_json_object_to_json_string(message));

  RETURN_IF_MISSING(trans, message, "transaction");

  RETURN_IF_MISSING(timestamp, message, "timestamp");
  RETURN_IF_MISSING(timestamp_us, timestamp, "us");

  int64_t ts = mtev_json_object_get_int64(timestamp_us);

  RETURN_IF_MISSING(trans_name, trans, "name");

  proto::Span span;
  span.set_operation_name(mtev_json_object_get_string(trans_name));

  RETURN_IF_MISSING(trans_type, trans, "type");
  RETURN_IF_MISSING(trace, message, "trace");

  mtev_json_object *service = NULL;
  const char *service_name = tm_service_name(message);
  int mv = tm_apm_server_major_version(message);

  mtev_json_object *context = NULL;
  if (mv == 6) {
    context = mtev_json_object_object_get(message, "context");
  } else if (mv > 6) {
    context = message;
  }
  service = mtev_json_object_object_get(context, "service");

  const char *trace_id = mtev_json_object_get_string(mtev_json_object_object_get(trace, "id"));
  proto::Process *p = span.mutable_process();
  p->set_service_name(service_name);

  struct trace_id tid;
  fill_trace_id(trace_id, &tid);

  span.set_trace_id(&tid, sizeof(tid));

  /* stick the trace id into a tag so we can use as a log correlation id */
  proto::KeyValue* trace_id_kv = span.add_tags();
  trace_id_kv->set_key("trace.id");
  trace_id_kv->set_v_type(proto::ValueType::STRING);
  trace_id_kv->set_v_str(trace_id);

  mtev_json_object *tags = mtev_json_object_object_get(context, mv == 6 ? "tags" : "labels");
  if (tags != NULL) {
    mtev_json_object_object_foreach(tags, tagname, tagvalue) {
      if (tagname != NULL && tagvalue != NULL) {
        proto::KeyValue* tag = span.add_tags();
        tag->set_key(tagname);
        tag->set_v_type(proto::ValueType::STRING);
        tag->set_v_str(mtev_json_object_get_string(tagvalue));
      }
    }
  }

  proto::KeyValue* kind = span.add_tags();
  kind->set_key("span.kind");
  kind->set_v_type(proto::ValueType::STRING);
  kind->set_v_str("server");

  const char *type = mtev_json_object_get_string(trans_type);
  if (strcmp(type, "request") == 0) {

    mtev_json_object *req = NULL, *resp = NULL;
    if (mv == 6) {
      req = mtev_json_object_object_get(context, "request");
      resp = mtev_json_object_object_get(context, "response");
    } else {
      mtev_json_object *http = mtev_json_object_object_get(message, "http");
      if (http == NULL) return;
      req = mtev_json_object_object_get(http, "request");
      resp = mtev_json_object_object_get(http, "response");
    }
    if (req == NULL || resp == NULL) return;

    mtev_json_object *status_code = mtev_json_object_object_get(resp, "status_code");
    mtev_json_object *method = mtev_json_object_object_get(req, "method");
    mtev_json_object *url = NULL;
    mtev_json_object *path = NULL;
    if (mv == 6) {
      url = mtev_json_object_object_get(req, "url");
      path = mtev_json_object_object_get(url, "pathname");
    } else if (mv > 6) {
      url = mtev_json_object_object_get(message, "url");
      path = mtev_json_object_object_get(url, "path");
    }

    if (method != NULL) {
      proto::KeyValue* methodt = span.add_tags();
      methodt->set_key("http.method");
      methodt->set_v_type(proto::ValueType::STRING);
      methodt->set_v_str(mtev_json_object_get_string(method));
    }

    if (path != NULL) {
      proto::KeyValue* urlt = span.add_tags();
      urlt->set_key("http.url");
      urlt->set_v_type(proto::ValueType::STRING);
      urlt->set_v_str(mtev_json_object_get_string(path));
    }

    if (status_code != NULL) {
      proto::KeyValue* sc = span.add_tags();
      sc->set_key("http.status_code");
      sc->set_v_type(proto::ValueType::INT64);
      sc->set_v_int64(mtev_json_object_get_int(status_code));
    }

    /* log the request and response info */
    proto::Log *log = span.add_logs();
    log->mutable_timestamp()->set_seconds(ts / 1000000);
    log->mutable_timestamp()->set_nanos((ts % 1000000) * 1000);

    mtev_json_object *req_headers = mtev_json_object_object_get(req, "headers");
    if (req_headers != NULL) {
      proto::KeyValue* f = log->add_fields();
      f->set_key("request.headers");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(req_headers));
    }

    mtev_json_object *resp_headers = mtev_json_object_object_get(resp, "headers");
    if (resp_headers != NULL) {
      proto::KeyValue* f = log->add_fields();
      f->set_key("response.headers");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(resp_headers));
    }

    mtev_json_object *socket = mtev_json_object_object_get(req, "socket");
    if (socket != NULL) {
      proto::KeyValue* f = log->add_fields();
      f->set_key("request.socket");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(socket));
    }
    if (url != NULL) {
      proto::KeyValue* f = log->add_fields();
      f->set_key("request.url");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(url));
    }
    mtev_json_object *env = mtev_json_object_object_get(service, "environment");
    if (env != NULL) {
      proto::KeyValue* e = p->add_tags();
      e->set_key("environment");
      e->set_v_type(proto::ValueType::STRING);
      e->set_v_str(mtev_json_object_get_string(env));
    }
    mtev_json_object *user = mtev_json_object_object_get(context, "user");
    if (user != NULL) {
      proto::KeyValue* e = p->add_tags();
      e->set_key("user");
      e->set_v_type(proto::ValueType::STRING);
      e->set_v_str(mtev_json_object_get_string(user));
    }
  }
  else if (strcmp(type, "page-load") == 0) {
    proto::KeyValue* kind = span.add_tags();
    kind->set_key("span.kind");
    kind->set_v_type(proto::ValueType::STRING);
    kind->set_v_str("client");

#if 0 // TODO: revisit RUM stuff
    RETURN_IF_MISSING(page, context, "page");

    mtev_json_object *marks = mtev_json_object_object_get(trans, "marks");
    mtev_json_object *user = mtev_json_object_object_get(context, "user");

    proto::KeyValue* paget = span.add_tags();
    paget->set_key("page");
    paget->set_v_type(proto::ValueType::STRING);
    paget->set_v_str(mtev_json_object_get_string(page));

    if (marks != NULL) {
      proto::KeyValue* markst = span.add_tags();
      markst->set_key("marks");
      markst->set_v_type(proto::ValueType::STRING);
      markst->set_v_str(mtev_json_object_get_string(marks));
    }
    if (user != NULL) {
      proto::KeyValue* usert = span.add_tags();
      usert->set_key("user");
      usert->set_v_type(proto::ValueType::STRING);
      usert->set_v_str(mtev_json_object_get_string(user));
    }
#endif
  }
  mtev_json_object *agent = NULL;
  if (mv == 6) {
    agent = mtev_json_object_object_get(service, "agent");
  } else{
    agent = mtev_json_object_object_get(context, "agent");
  }

  if (agent != NULL) {
    proto::KeyValue* e = p->add_tags();
    e->set_key("elastic_apm_agent.name");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(mtev_json_object_object_get(agent, "name")));

    e = p->add_tags();
    e->set_key("elastic_apm_agent.version");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(mtev_json_object_object_get(agent, "version")));
  }

  mtev_json_object *runtime = mtev_json_object_object_get(service, "runtime");
  if (runtime != NULL) {
    proto::KeyValue *e = p->add_tags();
    e->set_key("runtime.name");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(mtev_json_object_object_get(runtime, "name")));

    e = p->add_tags();
    e->set_key("runtime.version");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(mtev_json_object_object_get(runtime, "version")));
  }

  mtev_json_object *process = mtev_json_object_object_get(context, "process");
  if (process != NULL) {
    proto::KeyValue *e = p->add_tags();
    e->set_key("process.pid");
    e->set_v_type(proto::ValueType::INT64);
    e->set_v_int64(mtev_json_object_get_int64(mtev_json_object_object_get(process, "pid")));
  }

  mtev_json_object *system = mtev_json_object_object_get(context, mv == 6 ? "system" : "host");
  if (system != NULL) {
    proto::KeyValue *e = p->add_tags();
    e->set_key("system");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(system));
  }

  const char *id = mtev_json_object_get_string(mtev_json_object_object_get(trans, "id"));
  uint64_t sid = hex2uint64BE(id, strlen(id));
  span.set_span_id(&sid, sizeof(sid));
  span.set_flags(1); // 1 means sampled, 2 means DEBUG

  span.mutable_start_time()->set_seconds(ts / 1000000);
  span.mutable_start_time()->set_nanos((ts % 1000000) * 1000);

  int64_t duration_us = mtev_json_object_get_int64(mtev_json_object_object_get(mtev_json_object_object_get(trans, "duration"), "us"));
  span.mutable_duration()->set_seconds(duration_us / 1000000);
  span.mutable_duration()->set_nanos((duration_us % 1000000) * 1000);

  /* handle the span refs, a transaction can be a child if it's part of a distributed trans  */
  mtev_json_object *parent_json = mtev_json_object_object_get(message, "parent");
  if (parent_json) {
    proto::SpanRef *parent = span.add_references();
    parent->set_trace_id(&tid, sizeof(tid));
    const char *parent_hex = mtev_json_object_get_string(mtev_json_object_object_get(parent_json, "id"));
    uint64_t parent_id = hex2uint64BE(parent_hex, strlen(parent_hex));
    parent->set_span_id(&parent_id, sizeof(parent_id));
    parent->set_ref_type(proto::CHILD_OF);

    mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
    mtevL(debug_jaeger, "Jaeger TRANS %s, trace: %s, parent: %s\n", 
          id, trace_id, parent_hex);

  } else {
    mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
    mtevL(debug_jaeger, "Jaeger ROOT TRANS %s, trace: %s\n", 
          id, trace_id);
  }

  jaeger::api_v2::PostSpansRequest req;
  proto::Batch *batch = req.mutable_batch();
  proto::Span *batch_span = batch->add_spans();
  *batch_span = span;
  *batch->mutable_process() = span.process();

  jaeger_client.send(req);
}

extern "C" void jaegerize_span(mtev_json_object *message, const char *jaeger_url)
{
  const JaegerClient &jaeger_client = get_jaeger_client(jaeger_url);

  RETURN_IF_MISSING(span_json, message, "span");

  RETURN_IF_MISSING(timestamp, message, "timestamp");
  RETURN_IF_MISSING(timestamp_us, timestamp, "us");

  int64_t ts = mtev_json_object_get_int64(timestamp_us);
  const char *service_name = tm_service_name(message);
  int mv = tm_apm_server_major_version(message);

  RETURN_IF_MISSING(span_name, span_json, "name");

  proto::Span span;
  span.set_operation_name(mtev_json_object_get_string(span_name));

  const char *type = mtev_json_object_get_string(mtev_json_object_object_get(span_json, "type"));

  mtev_json_object *context = NULL;
  if (mv == 6) {
    context = mtev_json_object_object_get(message, "context");
  }
  else {
    context = message;
  }

  RETURN_IF_MISSING(service, context, "service");
  RETURN_IF_MISSING(trace, message, "trace");

  proto::Process *p = span.mutable_process();
  p->set_service_name(service_name);

  if (strncmp(type, "message_read", 12) == 0 || strncmp(type, "messaging", 9) == 0) {
    proto::KeyValue* kind = span.add_tags();
    kind->set_key("span.kind");
    kind->set_v_type(proto::ValueType::STRING);
    kind->set_v_str("server");

    proto::KeyValue* typet = span.add_tags();
    typet->set_key("type");
    typet->set_v_type(proto::ValueType::STRING);
    typet->set_v_str(type);

    mtev_json_object *subtype = mtev_json_object_object_get(span_json, "subtype");
    if (subtype != NULL) {
      proto::KeyValue* subtypet = span.add_tags();
      subtypet->set_key("subtype");
      subtypet->set_v_type(proto::ValueType::STRING);
      subtypet->set_v_str(mtev_json_object_get_string(subtype));
    }
  }
  else if (strncmp(type, "db", 2) == 0) {
    /* log the context info for database ops */

    proto::KeyValue* kind = span.add_tags();
    kind->set_key("span.kind");
    kind->set_v_type(proto::ValueType::STRING);
    kind->set_v_str("server");

    proto::Log *log = span.add_logs();
    log->mutable_timestamp()->set_seconds(ts / 1000000);
    log->mutable_timestamp()->set_nanos((ts % 1000000) * 1000);

    mtev_json_object *db = NULL;
    if (mv == 6) db = mtev_json_object_object_get(context, "db");
    else if (mv > 6) db = mtev_json_object_object_get(span_json, "db");
    if (db != NULL) {
      proto::KeyValue *f = log->add_fields();
      f->set_key("context.db");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(db));
    }

    mtev_json_object *subtype = mtev_json_object_object_get(span_json, "subtype");
    if (subtype != NULL) {
      proto::KeyValue *f = log->add_fields();
      f->set_key("subtype");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(subtype));
    }

    mtev_json_object *action = mtev_json_object_object_get(span_json, "action");
    if (action != NULL) {
      proto::KeyValue *f = log->add_fields();
      f->set_key("action");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(action));
    }

    mtev_json_object *stacktrace = mtev_json_object_object_get(span_json, "stacktrace");
    if (stacktrace) {
      char *st = build_stack_trace(stacktrace);
      proto::KeyValue *f = log->add_fields();
      f->set_key("stacktrace");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(st);
      free(st);
    }
  }
  else if (strcmp(type, "external.http") == 0 || strcmp(type, "ext.http.http") == 0 || strcmp(type, "external") == 0) {
    /* log the context info for external calls */
    proto::KeyValue* kind = span.add_tags();
    kind->set_key("span.kind");
    kind->set_v_type(proto::ValueType::STRING);
    kind->set_v_str("server");

    proto::Log *log = span.add_logs();
    log->mutable_timestamp()->set_seconds(ts / 1000000);
    log->mutable_timestamp()->set_nanos((ts % 1000000) * 1000);

    mtev_json_object *http = NULL;
    if (mv == 6) http = mtev_json_object_object_get(context, "http");
    else if (mv > 6) http = mtev_json_object_object_get(span_json, "http");
    if (http) {
      proto::KeyValue* f = log->add_fields();
      f->set_key("context.http");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(http));
    }

    mtev_json_object *stacktrace = mtev_json_object_object_get(span_json, "stacktrace");
    if (stacktrace) {
      char *st = build_stack_trace(stacktrace);
      proto::KeyValue *f = log->add_fields();
      f->set_key("stacktrace");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(st);
      free(st);
    }
  }
  else if (strcmp(type, "resource") == 0 || strcmp(type, "hard-navigation") == 0) {
    /* log the context info for client resource calls */
    proto::KeyValue* kind = span.add_tags();
    kind->set_key("span.kind");
    kind->set_v_type(proto::ValueType::STRING);
    kind->set_v_str("client");

    proto::KeyValue* typet = span.add_tags();
    typet->set_key("type");
    typet->set_v_type(proto::ValueType::STRING);
    typet->set_v_str(type);
  }
  else if (strcmp(type, "cache") == 0) {
    /* log the context info for cache ops */

    proto::KeyValue* kind = span.add_tags();
    kind->set_key("span.kind");
    kind->set_v_type(proto::ValueType::STRING);
    kind->set_v_str("server");

    proto::KeyValue* type = span.add_tags();
    type->set_key("span.type");
    type->set_v_type(proto::ValueType::STRING);
    type->set_v_str("cache");

    mtev_json_object *subtype = mtev_json_object_object_get(span_json, "subtype");
    if (subtype != NULL) {
      proto::KeyValue* st = span.add_tags();
      st->set_key("cache.type");
      st->set_v_type(proto::ValueType::STRING);
      st->set_v_str(mtev_json_object_get_string(subtype));
    }

    proto::Log *log = span.add_logs();
    log->mutable_timestamp()->set_seconds(ts / 1000000);
    log->mutable_timestamp()->set_nanos((ts % 1000000) * 1000);

    mtev_json_object *stacktrace = mtev_json_object_object_get(span_json, "stacktrace");
    if (stacktrace) {
      char *st = build_stack_trace(stacktrace);
      proto::KeyValue *f = log->add_fields();
      f->set_key("stacktrace");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(st);
      free(st);
    }
  }


  const char *trace_id = mtev_json_object_get_string(mtev_json_object_object_get(trace, "id"));
  struct trace_id tid;
  fill_trace_id(trace_id, &tid);
  span.set_trace_id(&tid, sizeof(tid));

  const char *id = mtev_json_object_get_string(mtev_json_object_object_get(span_json, mv == 6 ? "hex_id" : "id"));
  uint64_t sid = hex2uint64BE(id, strlen(id));
  span.set_span_id(&sid, sizeof(sid));
  span.set_flags(1); // 1 means sampled, 2 means DEBUG

  span.mutable_start_time()->set_seconds(ts / 1000000);
  span.mutable_start_time()->set_nanos((ts % 1000000) * 1000);

  int64_t duration_us = mtev_json_object_get_int64(mtev_json_object_object_get(mtev_json_object_object_get(span_json, "duration"), "us"));
  span.mutable_duration()->set_seconds(duration_us / 1000000);
  span.mutable_duration()->set_nanos((duration_us % 1000000) * 1000);

  /* handle the span refs */
  mtev_json_object *parent_json = mtev_json_object_object_get(message, "parent");
  if (parent_json) {
    proto::SpanRef *parent = span.add_references();
    parent->set_trace_id(&tid, sizeof(tid));
    const char *parent_hex = mtev_json_object_get_string(mtev_json_object_object_get(parent_json, "id"));
    uint64_t pid = hex2uint64BE(parent_hex, strlen(parent_hex));
    parent->set_span_id(&pid, sizeof(pid));
    parent->set_ref_type(proto::CHILD_OF);

    mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
    mtevL(debug_jaeger, "Jaeger SPAN %s, trace: %s, parent: %s\n", 
          id, trace_id, parent_hex);
  } else {
    mtevL(tm_error, "Invalid span, no parent, trace:  %s, span_id: %s\n", trace_id, id);
  }

  jaeger::api_v2::PostSpansRequest req;
  proto::Batch *batch = req.mutable_batch();
  proto::Span *batch_span = batch->add_spans();
  *batch_span = span;
  *batch->mutable_process() = span.process();

  jaeger_client.send(req);

}

extern "C" void jaegerize_error(mtev_json_object *message, const char *jaeger_url)
{
  const JaegerClient &jaeger_client = get_jaeger_client(jaeger_url);

  mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");

  RETURN_IF_MISSING(error_json, message, "error");

  RETURN_IF_MISSING(timestamp, message, "timestamp");
  RETURN_IF_MISSING(timestamp_us, timestamp, "us");

  const char *service_name = tm_service_name(message);
  int64_t ts = mtev_json_object_get_int64(timestamp_us);
  int mv = tm_apm_server_major_version(message);

  mtev_json_object *context = NULL;
  if (mv == 6) {
    context = mtev_json_object_object_get(message, "context");
  } else if (mv > 6) {
    context = message;
  }
  RETURN_IF_MISSING(trace, message, "trace");

  proto::Span span;

  mtev_json_object *culprit_json = mtev_json_object_object_get(error_json, "culprit");
  mtev_json_object *exception = mtev_json_object_object_get(error_json, "exception");
  mtev_json_object *first_exception = NULL;
  if (mv == 6) {
    first_exception = exception;
  } else if (mv > 6) {
    // in APM server 7.x, the "exception" is actually an array and can contain multiple
    int l = mtev_json_object_array_length(exception);
    if (l > 0) {
      first_exception = mtev_json_object_array_get_idx(exception, 0);
    }
  }

  mtev_json_object *exception_message = NULL, *exception_type = NULL;
  if (first_exception != NULL) {
    exception_message = mtev_json_object_object_get(first_exception, "message");
    exception_type = mtev_json_object_object_get(first_exception, "type");
  }

  /* the name of the operation will be chosen from this list of properties in priority order:
   * 
   * culprit
   * exception_message
   * exception_type
   * the string "unknown error"
   */
  if (culprit_json != NULL) {
    span.set_operation_name(mtev_json_object_get_string(culprit_json));
  } else if (exception_message != NULL) {
    span.set_operation_name(mtev_json_object_get_string(exception_message));
  } else if (exception_type != NULL) {
    span.set_operation_name(mtev_json_object_get_string(exception_type));
  } else {
    span.set_operation_name("unknown error");
  }

  proto::KeyValue* kind = span.add_tags();
  kind->set_key("error");
  kind->set_v_type(proto::ValueType::BOOL);
  kind->set_v_bool(true);

  proto::Log *log = span.add_logs();
  log->mutable_timestamp()->set_seconds(ts / 1000000);
  log->mutable_timestamp()->set_nanos((ts % 1000000) * 1000);

  if (first_exception != NULL) {
    if (culprit_json != NULL) {
      proto::KeyValue* f = log->add_fields();
      f->set_key("culprit");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(culprit_json));
    }

    if (exception_message != NULL) {
      proto::KeyValue *f = log->add_fields();
      f->set_key("message");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(exception_message));
    }

    mtev_json_object *exception_module = mtev_json_object_object_get(first_exception, "module");
    if (exception_module != NULL) {
      proto::KeyValue *f = log->add_fields();
      f->set_key("module");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(exception_module));
    }

    if (exception_type != NULL) { 
      proto::KeyValue *f = log->add_fields();
      f->set_key("type");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(mtev_json_object_get_string(exception_type));
    }

    mtev_json_object *stacktrace = mtev_json_object_object_get(first_exception, "stacktrace");
    if (stacktrace != NULL) {
      char *st = build_stack_trace(stacktrace);
      proto::KeyValue *f = log->add_fields();
      f->set_key("stacktrace");
      f->set_v_type(proto::ValueType::STRING);
      f->set_v_str(st);
      free(st);
    }
  }

  mtev_json_object *request = NULL, *response = NULL, *system = NULL;
  if (mv == 6) {
    request = mtev_json_object_object_get(context, "request");
    response = mtev_json_object_object_get(context, "response");
    system = mtev_json_object_object_get(context, "system");
  } else if (mv > 6) {
    mtev_json_object *http = mtev_json_object_object_get(message, "http");
    if (http) {
      request = mtev_json_object_object_get(http, "request");
      response = mtev_json_object_object_get(http, "response");
    }
    system = mtev_json_object_object_get(message, "host");
  }

  if (request != NULL) {
    proto::KeyValue* f = log->add_fields();
    f->set_key("request");
    f->set_v_type(proto::ValueType::STRING);
    f->set_v_str(mtev_json_object_get_string(request));
  }

  if (response != NULL) {
    proto::KeyValue* f = log->add_fields();
    f->set_key("response");
    f->set_v_type(proto::ValueType::STRING);
    f->set_v_str(mtev_json_object_get_string(response));
  }

  if (system != NULL) {
    proto::KeyValue* f = log->add_fields();
    f->set_key("system");
    f->set_v_type(proto::ValueType::STRING);
    f->set_v_str(mtev_json_object_get_string(system));
  }

  proto::Process *p = span.mutable_process();
  p->set_service_name(service_name);

  mtev_json_object *agent = NULL;
  if (mv == 6) {
    mtev_json_object *service = mtev_json_object_object_get(context, "service");
    agent = mtev_json_object_object_get(service, "agent");
  }
  else if (mv > 6) {
    agent = mtev_json_object_object_get(message, "agent");
  }

  if (agent != NULL) {
    proto::KeyValue *e = p->add_tags();
    e->set_key("elastic_apm_agent.name");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(mtev_json_object_object_get(agent, "name")));

    e = p->add_tags();
    e->set_key("elastic_apm_agent.version");
    e->set_v_type(proto::ValueType::STRING);
    e->set_v_str(mtev_json_object_get_string(mtev_json_object_object_get(agent, "version")));
  }

  const char *trace_id = mtev_json_object_get_string(mtev_json_object_object_get(trace, "id"));
  struct trace_id tid;
  fill_trace_id(trace_id, &tid);
  span.set_trace_id(&tid, sizeof(tid));

  const char *id = mtev_json_object_get_string(mtev_json_object_object_get(error_json, "id"));
  struct trace_id error_id;
  fill_trace_id(id, &error_id);
  uint64_t sid = error_id.low;
  span.set_span_id(&sid, sizeof(sid));
  span.set_flags(1); // 1 means sampled, 2 means DEBUG

  span.mutable_start_time()->set_seconds(ts / 1000000);
  span.mutable_start_time()->set_nanos((ts % 1000000) * 1000);

  span.mutable_duration()->set_seconds(0);
  span.mutable_duration()->set_nanos(10000000);

  /* handle the span refs */
  mtev_json_object *parent_json = mtev_json_object_object_get(message, "parent");
  if (parent_json) {
    proto::SpanRef *parent = span.add_references();
    parent->set_trace_id(&tid, sizeof(tid));
    const char *parent_hex = mtev_json_object_get_string(mtev_json_object_object_get(parent_json, "id"));
    uint64_t pid = hex2uint64BE(parent_hex, strlen(parent_hex));
    parent->set_span_id(&pid, sizeof(pid));
    parent->set_ref_type(proto::CHILD_OF);

    mtev_log_stream_t debug_jaeger = mtev_log_stream_find("debug/jaeger");
    mtevL(debug_jaeger, "Jaeger ERROR %s, trace: %s, parent: %s\n", 
          id, trace_id, parent_hex);
  }

  jaeger::api_v2::PostSpansRequest req;
  proto::Batch *batch = req.mutable_batch();
  proto::Span *batch_span = batch->add_spans();
  *batch_span = span;
  *batch->mutable_process() = span.process();

  jaeger_client.send(req);

}

