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
 * observability: enabled (boolean, default false), include_session_name (boolean,
 * default false), endpoint (HTTP(S) base URL),
 * traces_endpoint/logs_endpoint (HTTP(S) full URLs), service_name/service_version,
 * protocol/traces_protocol/logs_protocol (only http/protobuf),
 * traces_exporter/logs_exporter (otlp or none), headers/traces_headers/logs_headers,
 * resource_attributes (string maps or percent-encoded key=value lists).
 * Nonempty OTEL environment settings outrank config; signal-specific wins within
 * each source. OTEL_SERVICE_NAME overrides resource service.name; resource
 * service.name/version otherwise override their config fields.
 * OTEL_SDK_DISABLED=true suppresses both sinks, retaining identity.
 * Curl without ASYNCHDNS disables network export only.
 * Export strings use agent.redact.text; errors/missing module fail closed.
 * HTTP credentials are transport configuration, not exported attributes.
 * No historical free-form startup logs are retained or synthesized. Exported
 * logs are bounded span lifecycle events; configuration failures increment
 * configuration_errors without logging or exposing configuration values.
 * Fixed bounds and retry defaults follow specs/observability.md; queued backlog
 * drains on subsequent polls without a per-batch one-second delay.
 * Lifecycle local adapter decodes canonical LogRecord attributes into ordered,
 * typed log_event_structured() pairs, preserving repeated keys. Writes are
 * synchronous and best-effort; failure does not prevent native completion.
 * Identity/lifecycle ownership is independent of network enablement: normal
 * states receive contexts even with disabled/invalid network configuration.
 * enabled controls OTLP only; file_exporter (boolean, default true) independently
 * selects the best-effort local adapter using the existing log configuration.
 * This preserves enabled-mode local logging and enables offline correlation.
 * OTEL_SDK_DISABLED=true suppresses both sinks, not identity. Isolated benchmark
 * states still deny all capabilities, including contexts. Cleanup is terminal.
 * Both sinks consume canonical bounded records, never separate raw policies.
 * Poll/shutdown never write local diagnostic files. Explicit diagnostics() pulls
 * attempt a synchronous structured exporter.diagnostics log in the current scope,
 * without trace/span IDs. A recursion guard suppresses nested diagnostic writes;
 * log failure does not change the snapshot/counters. No async queue is provided.
 *
 * capstan.telemetry.start(name,parent?,attributes?) ->
 *   {trace_id,span_id,run_id,session_id} or nil
 * capstan.telemetry.end_span(context,ok,cancelled?,attributes?) -> boolean
 * capstan.telemetry.diagnostics() -> {dropped,rejected,failed,malformed,
 *   configuration_errors,queued,enabled}, or nil for isolated states.
 * The pull can perform log IO, but does no exporter polling; queued excludes
 * in-flight records. Numeric counters remain available after cleanup.
 * Captured run.id/mode/session.id are exported attributes, not resources;
 * session.name additionally requires include_session_name and persisted identity.
 * Returned session_id is empty when unscoped. Registry-owned parent snapshots
 * survive completion and caller mutation, retaining root run/session identity.
 * Lua on_run_start delivers this context before validation/hooks/transport for
 * CLI compatibility trace run.context, including offline runs; denied/unavailable
 * native context remains absent in compatibility output.
 * Runtime model/tool duration_ms is computed once for native and legacy observers;
 * native span envelope timestamps retain their own lifecycle boundary.
 * Names: agent.run, agent.model, agent.tool, run, model, tool, subagent,
 * compaction, title, completion_review;
 * other names become operation. IDs are lowercase hex. Parent IDs must be
 * nonzero hex. Context fields never control internally retained timestamps.
 * Denied/invalid calls do not raise argument errors; disabled sinks do not
 * deny identity in normal states. Lua allocator failure
 * remains subject to Lua's normal out-of-memory handling.
 */
/* Retain one fixed pair of native timestamps. A CLI context set before init
 * defers export until the first agent.run root: one correlated startup interval
 * ending at root start, shared by both sinks. Missing/isolated roots emit none;
 * invalid clocks omit the interval. Other modes retain the process marker. */
void telemetry_startup(void);
/* Adapter boundary: set before launching a root; NULL/empty session omits
 * persisted identity. Existing children retain their root's snapshot. */
void telemetry_set_context(const char *mode, const char *session_id,
                           const char *session_name);
void telemetry_init(lua_State *L, int isolated);
/* Pure exporter-result classification; curl_code is a CURLcode numeric value.
 * Returns a static exporter_dns/connect/tls/auth category, or exporter_transport
 * for other curl failures, exporter_http for other non-200 statuses, NULL for
 * CURLE_OK + HTTP 200. Curl failures take precedence over HTTP status.
 * Response decoding (protocol/rejected), configuration, retries and counters
 * remain owned by the exporter. No I/O, state mutation or Lua API. */
const char *telemetry_exporter_result_category(int curl_code, long http_status);
void telemetry_poll(void);
void telemetry_cleanup(void);
#endif
