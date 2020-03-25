#include "tm_hooks.h"

MTEV_HOOK_IMPL(elastic_apm_document, (mtev_json_object *top, mtev_json_object **result, int *ttl), void *, closure, 
               (void *closure, mtev_json_object *top, mtev_json_object **result, int *ttl), (closure, top, result, ttl));

MTEV_HOOK_IMPL(trace_transaction, (mtev_json_object *message), void *, closure, 
               (void *closure, mtev_json_object *message), (closure, message));

MTEV_HOOK_IMPL(threshold_fetch, (mtev_hash_table *thresholds), void *, closure, 
               (void *closure, mtev_hash_table *thresholds), (closure, thresholds));

