# Observability

## Native OpenTelemetry

Native network export is opt-in through `observability.enabled = true`; see
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
  encoding. Runtime loops poll the same native exporter. Legacy v1 callbacks
  remain format/publication adapters, not a second measurement owner; no path
  reconstructs spans from free-form log messages.
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
  Captured `session.id`, `run.id` and `mode` are span/log attributes, not resources.
  `session.name` is included only with `observability.include_session_name = true`
  and persisted session identity. Children inherit the root snapshot, including
  deferred work after session switching or parent completion.
- Exported logs contain `span.started` and `span.finished`, with trace/span
  correlation and `span.name`. Both include captured, allowlisted start attributes;
  finishes additionally include completion measurements, `outcome` and `cancelled`.
  The native attribute policy is shared with spans; retained start attributes are
  encoded caches, not references to mutable caller tables. Logs also contain
  one uncorrelated `runtime.started` startup event in non-CLI modes; CLI uses
  the correlated startup interval described below. Severity is INFO (9), except failed, non-cancelled finishes use
  ERROR (17). There is no raw-log exporter or historical free-form log replay.
- Span string attributes have a fixed allowlist: `operation`, `provider`,
  `model`, `profile`, `tool`, `purpose`, `subagent_id`; child runs are named
  `subagent` and include numeric `subagent_index` and `attempt` when known.
  `subagent_id` uses the same bounded redaction as all other strings.
  Native completion adds `outcome` and
  boolean `cancelled`. Failed, non-cancelled completion also accepts a closed
  vocabulary for `error.category`, never classification of raw error text. Only allowlisted finite nonnegative numeric measurements
  (counts, durations, token usage, stream timings and HTTP/curl statistics) pass.
  Names are `run`, `model`, `tool`, `agent.run`, `agent.model`, `agent.tool`,
  `subagent`, `compaction`, `title`, `completion_review`; others become `operation`.
- Export strings pass through canonical `agent.redact.text`, then UTF-8/control
  normalization. Redactor errors or a missing module use `[REDACTION_FAILED]`,
  never a weaker fallback. Redacted strings over 128 bytes or containing NUL
  become `[OVERSIZED]`, not truncated credential prefixes. Arbitrary attribute
  keys, prompts, reasoning, answers, arguments, commands, paths, URLs, headers,
  result bodies and raw exceptions are not automatically exported. Captured
  session identity is the explicit exception above, subject to the same bounded
  redaction policy. Resource attributes are explicit operator-supplied
  export data (not the span allowlist), also bounded and redacted; do not put
  sensitive data in them. No host, workspace or session resource detection runs.
- Local logging retains its existing content policy, redaction and rotation.
  The native lifecycle adapter adds local trace/span correlation using the
  captured session, but does not forward existing logs. It decodes the canonical
  bounded LogRecord attributes into an ordered JSONL `attributes` list of
  `{key,value}` pairs, preserving types and repeated keys without a second
  attribute policy. Its local write is synchronous; local write failure does
  not prevent OTLP completion/export. `observability.file_exporter` is an
  independent boolean, default `true`, selecting this offline lifecycle sink
  through existing log configuration. Normal states retain native IDs with OTLP
  disabled or invalid, even with both sinks off. `OTEL_SDK_DISABLED=true`
  suppresses both sinks, not identity. Strict isolated-state denial still applies.
- Network export is best-effort and disabled by default. Invalid transport settings
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
  Shutdown selects queued traces before lifecycle logs so a slow or retrying
  logs endpoint cannot consume the flush budget before completed roots get an
  export attempt. In-flight requests and pending retry backoff are preserved;
  they can still exhaust the shared deadline. Shutdown discards remaining
  records and unfinished spans into loss counters. Native receiver regression
  tests cover completed-root delivery with slow logs and logs Retry-After.
- Retry only connection failures and OTLP retryable HTTP 429/502/503/504, using
  1 s then 2 s backoff, or a longer Retry-After (seconds or HTTP date, capped at
  60 s). HTTP 200 partial success counts rejected records without retry;
  malformed success responses increment `malformed`. Delivery can duplicate
  after network loss. Other permanent failures count failed records.
- Before-config free-form logs are neither retained for export nor replayed.
  A fixed pair of native startup timestamps is retained until the first CLI root
  or cleanup. Non-CLI modes emit `runtime.started` only when network export is
  enabled, then discard the slot. Repeated startup calls do not grow the buffer.
  No startup configuration or free-form content is captured.
  Isolated benchmark Lua states cannot export, inspect diagnostics or finish
  another state's spans. Isolation is sticky, including saved closures.
  Configuration is process-scoped; session switching does not reset exporters.
  A libcurl build without asynchronous DNS disables export rather than risking
  blocking name resolution.

### Opt-in tool details

`observability.include_tool_details` is a strict boolean, default `false`.
With explicit `true`, tool instrumentation selects `shell.command` from shell
`command`, or scalar `tool.target` from `path` for `file_read`, `file_edit`,
`file_write`, `wiki_read`, `wiki_write`, `wiki_source_read`, and from `url` for
`fetch`. Unknown tools, non-string targets and bulk `paths` are omitted. There
is no serialization of bulk arguments, environment, stdin or output. Commands
and URLs can contain sensitive information: enable only for trusted sinks.

Lua selects fields from the effective routed arguments. Native export owns the
opt-in gate and the canonical redactor/128-byte bound, shared by OTLP spans,
OTLP lifecycle logs and the local structured adapter (including offline mode).
Oversized redacted values become `[OVERSIZED]`; redactor failure is fail-closed.
Existing free-form logs and the v1 compatibility trace do not change. Lua field
selection tests and independently decoded native OTLP tests cover default-off,
explicit enablement, invalid configuration, redaction and byte bounds.

### Network measurement mapping

The Lua adapter exports native HTTP cumulative `namelookup_elapsed_ms`,
`connect_elapsed_ms`, `appconnect_elapsed_ms`, `pretransfer_elapsed_ms`,
`starttransfer_elapsed_ms`, `total_ms`, and the non-overlapping phases described
below, under `transport.*`. Native `uploaded_bytes` / `downloaded_bytes` map to
`transport.upload_bytes` / `transport.download_bytes`; the native counters take
precedence over legacy adapter aliases when both are supplied. Missing phases
(for example with redirects) stay absent. Milestones are not additive phases.

### Session diagnostics migration (implemented subset and remaining gaps)

The primary analysis path is OTLP → Collector/storage → UI or storage API. No
local trace file is required for diagnostics. Implement one instrumentation and
context policy, with exporters as adapters, not independent measurement systems.
The existing `--trace-file` implementation below remains a strict v1 compatibility
adapter for published isolated benchmarks. Diagnostic consumers can instead use
native lifecycle logs or the backend; no legacy file is required. Runtime owns
completed model/tool counts and model/tool/permission/subagent totals once and
passes those exact values to native export and the v1 completion adapter.
Collector's file exporter supports server-side archival. The independent native
`file_exporter` serves offline correlation in existing local JSONL logs; both
sinks adapt one canonical encoded policy. It does not replace compatibility
trace publication.

Implemented session/root ownership and opt-in benchmark resource context:

- Session: `session.id` is stable; `session.name` is a redacted, opt-in snapshot
  captured at root launch. Equal names are disambiguated by ID. Rename affects
  subsequent roots, not historical records. Missing names remain absent.
- Root execution: `run.id` identifies one invocation, not the entire conversation.
  `trace_id` identifies its distributed tree; `span_id` identifies an operation.
  Children inherit captured session/run context, including deferred work after
  their parent finishes. No asynchronous lookup of the current session is allowed.
- Unsaved execution still has run/trace identity; omit persisted-session fields
  rather than inventing a saved session. CLI/TUI/ACP adapters provide mode and
  capture identity at their lifecycle boundary. Resource describes the process,
  never the changing session. IDs must not become metric labels.
- Diagnostic harness runs can opt into `run_eval.py --telemetry-context`.
  This passes `benchmark.attempt_id`, `benchmark.task`, `benchmark.comparison_id`,
  `benchmark.replicate_id`, `benchmark.config_id` and `benchmark.harness_sha256`
  through standard `OTEL_RESOURCE_ATTRIBUTES`, using the existing native resource
  exporter rather than a second ingestion API. Missing values are omitted;
  unrelated operator resources are preserved and stale `benchmark.*` replaced.
  These are process resources because one harness child executes one attempt;
  session/run identity remains captured span context. Values are bounded to 128
  safe ASCII characters; invalid identity fails before launching the agent.
  This opt-in does not enable export or discover configuration. Commands with
  `--benchmark` are rejected because that mode intentionally isolates native
  export. Default comparisons and scoring children are unchanged. The choice is
  recorded in metadata and the configuration hash; diagnostic runs must not be
  presented as equivalent telemetry-off comparisons. Real backend ingestion of
  these six fields is verified by the latest `benchmark-resources` fixture;
  browser comparison/task/replicate filtering and navigation are verified;
  no actual paired paid corpus comparison was run. External process wall/CPU/RSS remain harness-owned:
  runtime span duration is not process duration. Unknown comparator metrics are
  absent, never zero; failed, timed-out and partial attempts remain in reports.

Target structured events cover model/tool start and finish, retries with reason
and wait, permission waits/decisions, child scheduling, guard stops, errors,
compaction/title/review, and root completion. Each event uses explicit context,
result/category and applicable allowlisted measurements. Durations are monotonic
milliseconds, timestamps Unix nanoseconds on OTLP, sizes bytes, usage token
counts. Provider absence is not a measured zero. Overlapping children are shown
as parallel branches, never summed into parent wall time. Preparation and final
persistence need separate lifecycle coverage; unexplained gaps remain visible.

Implemented operation spans cover stream/subagent `retry` transitions with categorical
purpose and attempt where known; these are not backoff/wait measurements because
no agent retry delay policy exists. They also cover `permission_wait` and `permission_decision`, `subagent_queue`, and
`guard_stop`; their lifecycle logs carry the same fields. This is not a general
structured-event API or exhaustive instrumentation of every delay/error path.
Model/tool completion computes duration once and passes that exact value to both
native measurements and legacy observers; OTLP span envelope timestamps still
measure the native lifecycle boundary separately. `on_run_start(context)` runs
before validation, hooks and transport, allowing CLI `run.context` publication
at root start rather than completion, including offline compatibility traces.
It is absent when native context is denied or unavailable. `main` captures the
fixed native wall/monotonic timestamps before argument parsing. The first CLI
`agent.run` root consumes them into one shared `operation=startup` child span,
with the root's trace/run/session identity. Its monotonic duration ends exactly
at root start; the start timestamp is anchored backwards from that boundary to
avoid wall-clock corrections inverting the interval. Missing clocks, underflow,
missing/isolated roots omit the interval; cleanup discards unused timestamps.
No caller timestamp is accepted, no uncorrelated CLI marker is emitted, and the
root itself is not extended. Both local and network sinks use the same lifecycle
and attribute policy, including offline runs. CLI final persistence emits a shared
`operation` span (`operation=session_save`) parented to the retained native root
context after runtime completion. Its `duration_ms` is the exact legacy
`timing.session_save_ms` measurement; its outcome reflects saving, independently
of model success. No save span is synthesized for an unsaved/isolated run or
unavailable context. Instrumentation errors remain best-effort and cannot mask
save or requested trace-publication errors. The runtime root is not extended to
include persistence. TUI completion/title persistence uses the same adapter
(see below). Initial CLI session loading/saving is inside aggregate startup,
not individually attributed. Failures before root creation have no shared run
span; shutdown/export is outside runtime duration and remains process accounting.
Native identity is independent of both sinks; local lifecycle output is
independent of OTLP. Isolated benchmark states still deny context and export.

Coverage matrix (tests listed are coverage, not a claim they ran in this edit):

| Data | Runtime → native export | Collector → UI |
| --- | --- | --- |
| Resource service/version | Implemented | Fresh `build/grafana-acceptance-final.json`: PASS, 10 traces / 53 spans / 106 logs across real-backend fixture scenarios; not ten real CLI runs |
| Run/model/tool tree, explicit parents | Implemented; real CLI concurrent-child fixture included | Fresh artifact retrieves concurrent trace `e89b68e03e8be43170ba53c6e70d87eb`; parent separately verified browser session → concurrent trace `a41b9b544adbaa4f3de1b06edbfb6ab3` → Loki → Tempo |
| Start/completion fields on lifecycle logs | Implemented, independent decoder tests | Fresh artifact includes correlated logs; browser navigation verified by parent on the separate trace above, not independently repeated in this audit |
| HTTP milestones/phases and byte counters | Implemented adapter mapping | Actual Tempo values inspected; controlled loopback workload, not remote-provider performance evidence |
| Session name/ID and run/mode context | Native capture and real-binary mode tests | Fresh artifact includes named CLI sessions and native context/root fixtures; browser session selection verified by parent. Native TUI/ACP fixtures are not full interactive backend acceptance |
| Retry/permission/queue/guard operation spans | Runtime timeline and provider-tools tests; not exhaustive | Fresh real CLI concurrent and retry/error fixtures close the earlier parallel/retry gap; permission decision and controlled waits covered. Failure/cancel/timeout native fixtures do not establish real CLI cancel/timeout or every guard path |
| Exact shared model/tool durations and root-start callback | Lua/mode tests; offline compatibility `run.context` | CLI durations checked against real backend; does not establish migration of legacy consumers |
| Local structured lifecycle attributes | Serialization and native decoder tests; independent offline sink | Local sink only; deliberate absence of raw-log replay is a documented safety limitation, not missing lifecycle export |
| Early startup/final persistence intervals | Shared CLI startup interval and shared CLI/TUI completion persistence adapter implemented; `test/test_telemetry_modes.py` asserts post-root saves and exact CLI legacy save duration; selected-session test covers context/stale-session handling | Non-CLI startup is a marker, not a preparation interval. Initial CLI load/save is included in aggregate startup, not separate spans. Pre-root failure has no run span; runtime root excludes persistence and shutdown/export. Manual/debounced saves outside completion are deliberately unattributed; process wall/CPU/RSS remain harness-owned |
| Benchmark attempts → session/run/trace links | Sidecar/parser tests and offline native identity; six benchmark resource fields present in fresh `benchmark-resources` acceptance | Backend ingestion and browser comparison/task/replicate navigation are verified using fixture identities, not an actual paid paired corpus comparison |
| Export losses and error categories | `src/telemetry.c` exposes aggregate loss/configuration counters and the latest exporter `error.category` through explicit local pull, including DNS/connect/TLS/auth/HTTP classifications. These are not per-cause counters. Failed non-cancelled operation completions also support a closed error-category vocabulary; model transport measurements remain distinct from exporter diagnostics | No autonomous diagnostic queue or backend loss dashboard; explicit local pull is the implemented fallback, not autonomous failure reporting |
| Shared measurements and compatibility | Runtime owns root aggregates; native file/OTLP and C v1 adapters reuse them. `canonical_logs.py` maps explicit root totals into existing analyzer counts/breakdowns; real-binary trace-free test verifies values against OTLP. Missing old/forced-closure metrics stay absent | Additional backend checks `build/grafana-shared-summary-final.json` and `build/grafana-shared-children-final.json` verify common totals and exclusion of child work. Explicit v1 publication remains fail-closed; isolated comparisons keep their compatibility format |
| Backend outage/recovery | Fresh artifact records controlled HTTP 503 rejection and retry through the acceptance proxy | Logs: two attempts, one 503, one successful forward; traces: one successful attempt. This verifies controlled recovery into real storage, not a storage restart, prolonged outage, remote TLS/auth or production load |
| Comparable off/on overhead | `build/telemetry-modes-overhead.json`: ten alternating CLI pairs after warmup, per-PID CPU and peak RSS; median wall 108.807 → 113.371 ms (+4.564 ms, +4.20%); median RSS 29,016,064 → 29,163,520 bytes; final binary SHA-256 recorded in acceptance evidence | Loopback fixture with startup/local logs/shutdown included; not isolated instrumentation cost, production performance, or TUI/ACP latency. These are documented measurement limits, not absence of an overhead check |

Failure and safety: network export remains best-effort, bounded and isolated from
agent success. A compatibility file adapter must preserve explicitly requested
fail-closed publication until its users migrate. Filtering/redaction belongs to
the common policy, not per-destination implementations. Never auto-export prompts,
commands, arguments, answers, URLs, headers or raw exceptions. Export failure
must be diagnosable without relying solely on the failed exporter.

Acceptance requires independent OTLP decoding plus a real Collector and the
selected backend: two sessions, rename/switch, retry, concurrent children,
error/cancel/timeout and controlled delay. Search by session name + ID, select a
run, locate the delay and open correlated logs. Also measure comparable telemetry
off/on process wall/CPU/RSS. [Acceptance evidence](../examples/observability/acceptance.md)
records native, real CLI controlled-delay, and synthetic failure/cancellation
scenarios against real Tempo/Loki, plus a measured local overhead report. Native
baseline evidence is orchestrator-reported; CLI and failure artifacts were checked.
The short synthetic workload is not a production performance conclusion.
[Grafana browser evidence](../examples/observability/grafana.md#browser-validation)
additionally records dashboard import and a selected session timeline/attributes.
Real CLI parallel/retry transitions and relay-injected 503 recovery into storage
are verified, as are browser trace/log navigation and benchmark filters. No real
backend process restart, prolonged outage, production load or remote TLS/auth
handshake test is claimed. Cancellation includes native/runtime fixtures rather
than every live CLI transition. Mock tests alone do not establish these behaviors.

### Common root measurements

`agent.run` completion exports `request_count`, `tool_count`, `model_ms`,
`tool_ms`, `permission_wait_ms` and `subagent_wait_ms`. Runtime is the one
aggregation owner; C v1 callbacks only serialize events and consume the terminal
measurement snapshot. Permission time is clipped to the measured tool interval;
subagents contributes parent wait, never the sum of child work. With a known
root duration, runtime also emits `unattributed_ms` and `overlap_ms` from the
signed residual. Counts represent completed observed callbacks, not promises
that every remotely exported event arrived. Interrupted operations are not
assigned invented durations. Forced owner/shutdown closure snapshots completed
run counters before native span export, including child runs; duration and
residual totals remain absent when the run did not complete normally.
`canonical_logs.py` adapts only supplied fields, including historical logs with
missing totals; it never reconstructs them from timestamps or children.

### Diagnostics

`capstan.telemetry.diagnostics()` returns an in-memory snapshot: `dropped`,
`rejected`, `failed`, `malformed`, `configuration_errors`, `queued` (numeric
counters/gauge), `enabled` (boolean), and optional `error.category` (latest
exporter failure: configuration, DNS, connect, TLS, auth, transport/HTTP fallback,
protocol or rejection). Only categorical constants, never response/error text,
are exposed. `queued` excludes in-flight records.
Isolated states receive `nil`. Counters remain readable after cleanup, including
shutdown losses. Each permitted explicit pull also attempts a synchronous local
`telemetry` / `exporter.diagnostics` structured event in the current log scope,
without trace/span IDs. Log failure does not change counters or the returned
snapshot; a recursion guard prevents nested diagnostic writes. This call can do
log I/O and is not a nonblocking API. Export polling/shutdown themselves do not
write diagnostic files, invoke this adapter or print stderr. There is no
autonomous asynchronous local diagnostic queue.

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

### Completion persistence adapter

CLI completion and TUI completion/title saves use the same C adapter into
`agent.telemetry.start/finish`; it owns no independent identity or export policy.
Completion callbacks pass the explicit native run-context snapshot, including
on failures. TUI submissions also capture their session ID independently of
telemetry, so a late callback cannot attribute a different session's save to
that run. No context means no attributed persistence span, not a guessed parent.
Compaction persists under its own run; generated titles persist under the title
run, whose explicit parent is the originating conversation run. Debounced and
manual saves outside completion remain unattributed. Save errors retain retryable
dirty state; TUI reports them and CLI preserves its existing failed-result policy.
Adapter/export errors are best-effort and do not prevent the actual save.

Coverage: agent selected-session test checks explicit context delivery, stale
session rejection and disk restoration; cross-mode telemetry tests cover CLI/TUI.
