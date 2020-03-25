#ifndef TM_HOOKS_H
#define TM_HOOKS_H

#include <mtev_hash.h>
#include <mtev_hooks.h>
#include <mtev_json_object.h>


/**
 * Called when a new elastic APM document arrives to tracemate.  Allows specialized processing of the document
 * and allows the hook implementer to return a new JSON document (result) and a ttl.
 *
 * @MLB uses this for special handling of transaction documents on their way to BigQuery for one of our services.
 */
MTEV_HOOK_PROTO(elastic_apm_document, (mtev_json_object *top, mtev_json_object **result, int *ttl),
                void *, closure, (void *closure, mtev_json_object *top, mtev_json_object **result, int *ttl));

/**
 * Called during the processing of a transaction document, implementers can decide to send this document wherever they want.
 *
 * @MLB uses this to send data to BigQuery for one of our services.
 */
MTEV_HOOK_PROTO(trace_transaction, (mtev_json_object *message),
                void *, closure, (void *closure, mtev_json_object *message));

/**
 * Called by the maintenance jobq every 60 seconds.  This can be used to look up jaeger tracing threshold times
 * in milliseconds in some external systems.  @MLB uses this to lookup trace thresholds in consul.
 *
 * Hash table should be filled with:
 *   Key is the full service name, e.g.: `bdata-statsapi-gaming-prod-us-east-4`
 *   Value is a string which contains the threshold (in milliseconds as a string) over which the transaction (and
 *     all its children) will be sent to jaeger for tracing.
 *
 * You can use a special key: 'default' to indicate the default threshold for any service that is not otherwise
 * configured.
 *
 * Out of the box you can configure threshold times using the configuration file of the application, but sometimes
 * you want fine control over these thresholds while the app is running without having to perform a configuration
 * deployment.  This lets you write some code to lookup (or programmatically determine) the thresholds for each
 * service at runtime.
 */
MTEV_HOOK_PROTO(threshold_fetch, (mtev_hash_table *thresholds),
                void *, closure, (void *closure, mtev_hash_table *thresholds));


#endif
