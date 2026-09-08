#ifndef CAPSTAN_TELEMETRY_H
#define CAPSTAN_TELEMETRY_H
#include <lua.h>
/* Main-thread only. Initialize after capstan.config and agent.redact are usable;
 * poll from every runtime loop; cleanup before curl global cleanup/log cleanup.
 * Configuration is process-scoped; isolated states cannot use the exporter or
 * consume another state's spans. Isolation remains sticky on re-registration.
 * Isolated initialization does not consume process configuration ownership.
 * Closures retain a registry-owned context; isolation also revokes previously
 * saved closures. Spans require both the originating Lua state and context
 * generation, so recycled state addresses cannot finish old spans. End spans
 * before closing their state. Cleanup is terminal and idempotent.
 *
 * observability: enabled (boolean, default false), endpoint (HTTP(S) base URL),
 * traces_endpoint/logs_endpoint (HTTP(S) full URLs), service_name/service_version,
 * protocol/traces_protocol/logs_protocol (only http/protobuf),
 * traces_exporter/logs_exporter (otlp or none), headers/traces_headers/logs_headers,
 * resource_attributes (string maps or percent-encoded key=value lists).
 * Nonempty OTEL environment settings outrank config; signal-specific wins within
 * each source. OTEL_SERVICE_NAME overrides resource service.name; resource
 * service.name/version otherwise override their config fields.
 * OTEL_SDK_DISABLED=true forces off. Curl without ASYNCHDNS disables export.
 * Export strings use agent.redact.text; errors/missing module fail closed.
 * HTTP credentials are transport configuration, not exported attributes.
 * No historical free-form startup logs are retained or synthesized. Exported
 * logs are bounded span lifecycle events; configuration failures increment
 * configuration_errors without logging or exposing configuration values.
 * Fixed bounds and retry defaults follow specs/observability.md; queued backlog
 * drains on subsequent polls without a per-batch one-second delay.
 * Poll/shutdown never write local diagnostic files. diagnostics() exposes
 * numeric process counters to permitted states, including shutdown losses.
 * Proposed parent log adapter: log_try_enqueue_numeric(snapshot), copying into
 * a bounded in-memory queue without filesystem IO, locks, Lua calls or waiting;
 * return failure immediately when full. A separate owner drains that queue.
 * No such log API is assumed or called here.
 *
 * capstan.telemetry.start(name,parent?,attributes?) -> {trace_id,span_id} or nil
 * capstan.telemetry.end_span(context,ok,cancelled?,attributes?) -> boolean
 * capstan.telemetry.diagnostics() -> {dropped,rejected,failed,malformed,
 *   configuration_errors,queued,enabled}, or nil for isolated states.
 * This snapshot does no IO, redaction or polling; queued excludes in-flight
 * records. Numeric counters remain available after cleanup.
 * Names: run, model, tool, subagent, compaction, title, completion_review;
 * other names become operation. IDs are lowercase hex. Parent IDs must be
 * nonzero hex. Context fields never control internally retained timestamps.
 * Disabled/invalid calls do not raise argument errors. Lua allocator failure
 * remains subject to Lua's normal out-of-memory handling.
 */
/* Retain one fixed, timestamp-only startup event until configuration loads. */
void telemetry_startup(void);
void telemetry_init(lua_State *L, int isolated);
void telemetry_poll(void);
void telemetry_cleanup(void);
#endif
