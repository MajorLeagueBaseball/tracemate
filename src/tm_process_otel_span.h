#ifndef TM_PROCESS_OTEL_SPAN_H
#define TM_PROCESS_OTEL_SPAN_H

#include <rdkafka.h>

#ifdef __cplusplus
extern "C"
#endif
bool otel_process_jaeger_span(rd_kafka_message_t *m, topic_stats_t *ts);

#endif
