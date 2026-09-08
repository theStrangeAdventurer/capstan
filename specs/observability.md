# Observability

## Native OpenTelemetry

Native export is opt-in through `observability.enabled = true`; see
[configuration and environment precedence](config.md#native-opentelemetry).
The existing local formats below remain unchanged.

- Transport: OTLP/HTTP protobuf for traces and logs, through existing libcurl;
  no new dynamic dependencies and no general-purpose SDK claim. Metrics are out
  of scope. A Collector is recommended but compatible direct endpoints work.
- The shared `agent/runtime.lua` execution paths supply instrumentation for CLI,
  TUI and ACP. `agent/telemetry.lua` owns lifecycle, explicit parent context,
  descendant cleanup and allowlisted measurement adaptation. `src/telemetry.c`
  owns process configuration, IDs, state ownership, bounded records, final
  attribute filtering and curl transport; `src/otlp_wire.c` owns protobuf wire
  encoding. Runtime loops poll the same native exporter. Legacy observers and
  trace output remain independent; none reconstruct spans from log messages.
- One top-level execution is one trace, including in TUI. Session identity is
  captured at start, not looked up when asynchronous work completes. Child runs
  inherit explicit parent context. Model attempts and tools get separate spans;
  compaction, title generation and completion review have distinct purposes.
- Lua lifecycle ownership settles descendants on terminal success, error or
  cancellation, including protected exceptions, and ignores repeated finishes.
  Abrupt process death cannot guarantee delivery; native cleanup counts unfinished
  spans as dropped. Cancellation has `cancelled=true`, `outcome=cancelled` and
  unset span status, not a fabricated transport error. Success/error set OK/ERROR.
  Unknown measurements remain absent, not zero.
- Records carry resource (`service.name`, `service.version`), instrumentation
  scope (`capstan.native`, version `1`), timestamps and IDs. Trace IDs are 16
  random bytes and span IDs 8 random bytes, never all-zero, rendered as lowercase
  hex in Lua. Parent context is explicit, not inferred from process-global work.
  Session identity is captured locally and inherited from the parent, including
  deferred work after session switching; it is never an OTLP attribute.
- Exported logs contain `span.started` and `span.finished`, with trace/span
  correlation, plus one uncorrelated `runtime.started` startup event. Severity is INFO (9), except failed, non-cancelled finishes use
  ERROR (17). There is no raw-log exporter or historical free-form log replay.
- Span string attributes have a fixed allowlist: `operation`, `provider`,
  `model`, `profile`, `tool`, `purpose`; native completion adds `outcome` and
  boolean `cancelled`. Only allowlisted finite nonnegative numeric measurements
  (counts, durations, token usage, stream timings and HTTP/curl statistics) pass.
  Names are `run`, `model`, `tool`, `agent.run`, `agent.model`, `agent.tool`,
  `subagent`, `compaction`, `title`, `completion_review`; others become `operation`.
- Export strings pass through canonical `agent.redact.text`, then UTF-8/control
  normalization. Redactor errors or a missing module use `[REDACTION_FAILED]`,
  never a weaker fallback. Redacted strings over 128 bytes or containing NUL
  become `[OVERSIZED]`, not truncated credential prefixes. Arbitrary attribute
  keys, prompts, reasoning, answers, arguments, commands, paths, URLs, headers,
  result bodies and raw exceptions are not automatically exported. Session IDs
  and titles stay local. Resource attributes are explicit operator-supplied
  export data (not the span allowlist), also bounded and redacted; do not put
  sensitive data in them. No host, workspace or session resource detection runs.
- Local logging retains its existing content policy, redaction and rotation.
  The native lifecycle adapter adds local trace/span correlation using the
  captured session, but does not forward existing logs. Its local write is
  synchronous; local write failure does not prevent OTLP completion/export.
- Export is best-effort and disabled by default. Invalid telemetry settings
  disable the exporter and increment `configuration_errors`, without printing
  settings or diagnostics to stderr. Explicit `--trace-file` remains fail-closed
  and retains `capstan.trace.v1` unchanged.
- The exporter uses a separate curl multi handle: no spinner, foreground cancel,
  recursive logging, stdout/JSON/ACP output or effect on agent exit status.
  TLS verification is mandatory; redirects are disabled to protect headers.
- Fixed bounds (not configurable): 1,024 live spans, 1,024 queued records,
  4 MiB queued bytes, 256 KiB payloads, 128 records per batch, 8 KiB attributes
  per start/end, 16 KiB encoded resource and 8 KiB response. A 1 s idle/initial
  batching delay does not delay each backlog batch; subsequent polls drain it.
  One request is in flight, with a 5 s request/connect timeout, three total
  delivery attempts and a 2 s shutdown flush budget. Queue overflow drops new
  records; oversized records are dropped, never split into invalid protobuf.
  Shutdown discards remaining records and unfinished spans into loss counters.
- Retry only connection failures and OTLP retryable HTTP 429/502/503/504, using
  1 s then 2 s backoff, or a longer Retry-After (seconds or HTTP date, capped at
  60 s). HTTP 200 partial success counts rejected records without retry;
  malformed success responses increment `malformed`. Delivery can duplicate
  after network loss. Other permanent failures count failed records.
- Before-config free-form logs are neither retained for export nor replayed.
  A single timestamp-only startup slot emits `runtime.started` after configuration
  enables export; otherwise it is discarded. Repeated startup calls do not grow
  the buffer. No startup configuration or free-form content is captured.
  Isolated benchmark Lua states cannot export, inspect diagnostics or finish
  another state's spans. Isolation is sticky, including saved closures.
  Configuration is process-scoped; session switching does not reset exporters.
  A libcurl build without asynchronous DNS disables export rather than risking
  blocking name resolution.

### Diagnostics

`capstan.telemetry.diagnostics()` returns a nonblocking in-memory snapshot:
`dropped`, `rejected`, `failed`, `malformed`, `configuration_errors`, `queued`
(numeric counters/gauge) and `enabled` (boolean). `queued` excludes in-flight
records. Isolated states receive `nil`. Counters remain readable after cleanup,
including shutdown losses. The snapshot does no polling, redaction or I/O;
export polling and shutdown write no diagnostic files or stderr. There is no
implemented asynchronous local diagnostic-queue adapter.

### Acceptance checks

Pure-C tests cover encoding, limits and delivery decisions; Lua tests cover
lifecycle and concurrent child correlation. Decode payloads independently and
send them to an actual Collector. Exercise unavailable endpoints, queue overflow,
cancellation, redaction and partial success. Run affected C/Lua/build tests and
legacy trace validation; do not claim Collector compatibility from mocks alone.

## Structured headless traces

`capstan run --trace-file PATH` writes a newline-delimited JSON trace independently
of stdout, stderr, and runtime logs. Capstan atomically reserves
`PATH.partial` with mode `0600`; an existing partial is treated as an active or
stale run and blocks reuse instead of being deleted. The `.partial` suffix is
reserved and cannot itself be used as a target, preventing one trace target from
colliding with another run's partial file. An older completed trace at
`PATH` remains available until the new partial is flushed, closed, and atomically
renamed over it. A killed or interrupted run leaves its partial file and does not
replace the older published result. Trace creation and `run.started`
happen before prompt loading, workdir/workspace validation, plugin setup, and
session setup. Handled startup failures publish `run.finished`; an abrupt
startup termination still leaves diagnostic events in the partial file. The
parent directory must either be non-writable by group/other users or have the
sticky bit (as `/tmp` normally does); unsafe shared directories are rejected
before the partial is created.

Each record uses schema `capstan.trace.v1` and contains a process-local sequence,
wall timestamp in milliseconds, monotonic elapsed run time, run ID, event name,
and a structured `data` object. Version 1 records no prompts, model output, tool
arguments, tool result content, headers, URLs, or credentials.

A published trace has these invariants:

- sequence values are contiguous and start at one;
- every event has the same run ID;
- the last record is the only `run.finished` record, and no other event can be
  published through the terminal-event API;
- the reserved partial remains a single-link regular file owned by the writer
  throughout recording and publication;
- `run.finished.data.ok` and `intended_exit_code` describe the run result after
  session persistence and before durable trace publication;
- explicit trace creation or any failure before atomic publication makes the CLI
  run fail; after a successful rename the trace is published, while directory
  `fsync` is a best-effort crash-durability step and cannot retroactively make a
  visible target unpublished.

Events cover model requests and tool calls. Tool observer callbacks preserve the
original JSON string in `arguments` and expose routed/decoded arguments in
`effective_arguments`, so existing callback consumers remain compatible while
telemetry reflects the operation actually executed. Trace observer callbacks are
fail-closed: a write failure stops the agent through its normal completion path
before further model or tool work, rather than unwinding the Lua runtime or
silently publishing an incomplete trace. Model start records the effective
provider, model, profile, reasoning effort, reasoning-continuity mode, request
body size, and cycle purpose (`agent`, `empty_response_retry`, or
`completion_review`). Model completion includes duration, first
output/reasoning/text/tool latency, provider usage, output sizes, HTTP and curl
status, actual byte counts, chunk and redirect counts. Optional latency,
usage, and transport values are JSON `null` when unavailable; zero is reserved
for a measured zero. `first_output_ms` marks the first non-empty semantic model
output even when a provider adapter buffers it; `first_text_ms` is marked only
after filtering and buffering produce non-empty visible text.

HTTP metadata distinguishes cumulative curl milestones from phases. Milestones
are named `*_elapsed_ms`; for requests without redirects, non-overlapping phases
are `dns_ms`, `tcp_connect_ms`, `tls_handshake_ms`, `request_setup_ms`,
`upload_and_server_wait_ms`, and `download_ms`. The upload and server-wait phase
is deliberately not called pure server latency because curl's pre-transfer to
start-transfer interval can include request-body upload. Derived phases are
omitted when redirects make them cumulative across multiple requests.
`ttfb_ms` remains the cumulative time to first response byte.

The final event reports turns, request/tool counts, duration, and a breakdown of
`model_ms`, ordinary `tool_ms`, `subagent_wait_ms`, and `unattributed_ms`.
Subagent wall time is deliberately not reported as ordinary tool execution.
Version 1 does not claim child model-work totals or parallel critical paths;
those require child run/span identifiers in a future additive schema revision.

## Runtime logs

Persisted runtime logs use JSONL schema `capstan.log.v1`. Each event is assembled
in memory, redacted, and appended with one complete write operation. Rotation and
append are protected by a stable per-log lock file so concurrent Capstan
processes cannot interleave records or race rotation. Rotation first snapshots
the complete next archive set into a private transaction directory; once marked
ready, an interrupted publication is resumed under the same lock before the
next append or `/logs` read. Published archive/current renames are synchronized
to both the transaction and parent log directories before the recovery manifest
is removed.

`/logs` renders JSONL as compact single-line text and also reads the current
day's legacy `.log` file, rotated legacy archives, and rotated JSONL archives.
Embedded newlines and terminal control characters are flattened or escaped so
message content cannot imitate separate records or terminal instructions. The
reader uses the C runtime's allowlisted, no-symlink tail API while holding the
same lock as writers. Lock acquisition returns the exact daily path protected by
that lock, so a date rollover between path discovery and locking cannot mix two
days. It keeps at most 500 recent records in memory and ignores an unterminated
tail. Malformed and legacy records remain visible.

## Benchmark integration

The Polyglot command template may use `{trace_file}`. The harness stores either
the published trace or a surviving `.partial` path and validates JSON parsing,
sequence continuity, run-ID consistency, terminal-event placement, and paired
model/tool lifecycle identities before copying terminal telemetry into
`results.json`.

`analyze_traces.py` uses the harness's canonical `agent.seconds` wall time, with
compatibility fallbacks for foreign result formats. Wall time includes timeout,
crash, and failed runs. Internal breakdowns separately report complete-trace
coverage and the stricter coverage of rows containing every displayed metric.
Missing task rows count against expected repetitions. Comparative deltas pair
only matching stable `replicate_id` and explicit `comparison_id` values with
identical suite, corpus commit, task matrix, and agent/test timeouts. A report
with a comparator rejects different configurations instead of displaying their
wall times in the same row. Unlabeled legacy runs remain analyzable on their own
but are never paired across result directories. Missing comparator or telemetry
values are shown as `N/A`, never as zero.
Timeout and nonzero-exit status fields are validated for consistency. A complete
trace whose terminal `ok` or `intended_exit_code` contradicts the observed
process result is retained with status `inconsistent` but excluded from internal
breakdown averages; the report shows that exclusion count explicitly. The report
also shows timeouts, agent errors, and missing repetitions separately for both
Capstan and the comparator. Harness task IDs must be unique and agent/test
timeouts must be positive before any task starts.

## Constraints

- JSON result output remains unchanged.
- Runtime logging is best-effort; explicitly requested traces fail closed.
- Raw content is deliberately excluded. Future content capture must be a
  separate explicit sensitive mode with canonical redaction.
- Schema changes must be additive or use a new schema version.

## Tests

`make test` covers CLI parsing, JSONL trace escaping, permissions, atomic publish,
and terminal uniqueness. `make test-http-lua` covers the Lua callback contract,
runtime logs, legacy `/logs`, and HTTP transport behavior. Python tests cover
trace validation and benchmark analysis. `make test-build` verifies embedded
runtime assets and the standalone binary.
