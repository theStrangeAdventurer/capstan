# Runtime Logs

## Behavior

Before a session is selected, Capstan writes process-level runtime events to:

```text
$XDG_STATE_HOME/capstan/logs/YYYY-MM-DD.jsonl
```

When `XDG_STATE_HOME` is not set, the fallback is
`~/.local/state/capstan/logs/YYYY-MM-DD.jsonl`.

Once a TUI session is active, or a headless run supplies `--session-id`, events
are isolated under that stable session ID:

```text
$XDG_STATE_HOME/capstan/logs/sessions/<session-id>/YYYY-MM-DD.jsonl
```

Session titles are not used as paths because generated titles can change.
Explicit CLI session IDs remain unchanged and therefore provide predictable log
paths. Switching TUI sessions changes the active log scope; `/logs` and the
`logs` model tool consequently read only the active session's current log.

Each physical line is one JSON object using schema `capstan.log.v1`. It includes
an RFC 3339 UTC timestamp with milliseconds, level, category, redacted message,
and the stable session ID when scoped:

```json
{"schema":"capstan.log.v1","timestamp":"2026-06-19T15:30:00.123Z","level":"info","category":"tool","session_id":"my bench","message":"call name=shell target=/project"}
```

JSONL is the canonical persisted format. `/logs` parses it and renders a compact
human-readable view; malformed or legacy plain-text lines remain visible verbatim.

The `/logs [n]` command displays the last `n` log lines in the conversation.
The same plugin exposes a `logs` model tool so the agent can inspect recent
runtime events when debugging failed tools, plugins, hooks, or API calls. If
`n` or `limit` is omitted, it shows the last 80 lines. The maximum is 500 lines.

Runtime logging honors `CAPSTAN_LOG_LEVEL`. Supported values are `error`, `warn`,
`info`, `debug`, and `trace`. The default is `info`, which keeps high-signal
lifecycle, tool, permission, and error events. `debug` adds low-level
stream/tool-call reconstruction events. `trace` also enables raw SSE/event
payload logging.

Log files rotate independently inside each scope. If the current file reaches
10 MiB before the day changes, Capstan renames it to `.1.jsonl` and keeps up to
five same-day archives:

```text
logs/2026-06-27.jsonl
logs/2026-06-27.1.jsonl
logs/2026-06-27.2.jsonl
```

Before a message is written, the logger redacts common secret shapes through the
canonical Lua redactor in `agent/redact.lua`: sensitive HTTP headers,
token/password key-value pairs, environment variables whose names contain
credential markers, and additive `redaction` rules from
`~/.config/capstan/config.lua`; see [Config](config.md). Built-in redaction is
not disabled by config.

Log messages are also normalized to valid UTF-8. Invalid byte sequences are
replaced before persistence, so reading logs back through the model tool cannot
poison a later JSON provider request.

`[REDACTED]` means the original value exists but was hidden. Agents must not
interpret it as the literal stored value, and must not probe secret values by
printing environment variables, headers, config fields, or credentials. Presence
checks should print only boolean/status information.

## Logged Events

- Agent request start: provider, model, message count, tool count, depth, and
  run kind (`orchestrator` or `subagent`)
- Tool names sent to the model
- Last outbound message role and compact content preview
- API stream request: endpoint, message count, tool count
- Non-2xx transport diagnostics: bounded provider body detail when present,
  plus safe request identifiers from response headers. Empty bodies are logged
  explicitly instead of being reported as an unexplained status code.
- Stream summary: SSE event count, raw byte count, chunk counters, final tool
  call count, text/reasoning byte counts
- Warnings for empty or reasoning-only responses, incomplete tool calls, and
  responses that combine assistant text with tool calls. These diagnostics log
  byte counts and call counts without duplicating the full assistant text.
- Tool-call stream deltas at `debug` level: index, id, accumulated name, and
  accumulated argument byte count
- Final reconstructed tool calls with id, name, and compact arguments
- Assistant text when the stream completes without tool calls
- Tool calls received from the model, including the byte count of any assistant
  text carried beside them
- Tool call name, permission target, and raw JSON arguments
- Shell tool calls log a redacted `display` label, redacted JSON arguments, and
  the full redacted command string used for execution. Curl commands may be
  summarized in the UI status as `curl <url>`, but runtime logs keep the full
  redacted command for later debugging. C-side shell start/done logs pass
  through the same redaction layer.
- Subagent starts: task index, task id, current provider, selected model, and a
  compact prompt preview.
- Subagent child-run diagnostics: task index, task id, child depth, effective
  max turns, effective tool count, and effective tool names.
- [Permission](permissions.md) checks and prompt decisions
- Tool completion and result size
- Shell plugin results at `info`: exit code, timeout notice, stdout and stderr,
  for manual and model calls. Output is redacted and UTF-8 sanitized, bounded by
  `tool_output.max_bytes` / `tool_output.max_lines`, and explicitly marked when
  truncated. One result event is emitted per execution, even in silent runs.
- Invalid UTF-8 replacement counts and model-bound tool-result truncation
- Tool handler failures, including compact diagnostic text
- Tool guard stops under the `tool_guard` category when the runtime aborts a
  runaway loop before the next tool execution
- Plugin load/reload failures from `~/.config/capstan/plugins/*.lua`
- Continuation after tool results
- [Hook](hooks.md) errors with stage and source

Set `CAPSTAN_LOG_LEVEL=trace` to include raw SSE chunks and parsed SSE event payloads
in the log. This is intentionally opt-in because raw stream logs can become
large and may include full model output. Raw stream logs still pass through the
best-effort redactor, but this mode should be treated as sensitive debug output.

## Native OpenTelemetry boundary

Optional [native OTLP](observability.md#native-opentelemetry) network export is
disabled by default and requires `observability.enabled = true`. Independent
local lifecycle output uses boolean `observability.file_exporter`, default `true`,
and the existing log configuration. `OTEL_SDK_DISABLED=true` suppresses both
native sinks but retains offline trace/span/run identity in normal states; it
does not disable existing free-form runtime logs. `CAPSTAN_LOG_LEVEL=trace`
does not enable network export or make raw SSE/free-form messages exportable.
Before-config free-form logs remain local: they are not buffered for export,
replayed or converted into synthetic startup events by the native exporter.

OTLP logs contain bounded `span.started` / `span.finished` lifecycle
messages, severity and trace/span IDs, plus `span.name` and the span's captured
allowlisted start attributes. Finish logs additionally carry completion
measurements, `outcome` and `cancelled`. The same native attribute filter and
canonical redactor govern spans and logs; no free-form message parsing is used.
Non-CLI modes emit one uncorrelated `runtime.started` event only to enabled OTLP
logs; file-only non-CLI operation does not emit this marker. CLI instead consumes
the startup timestamps into a correlated `operation=startup` interval for either
sink. When `file_exporter` is enabled, native lifecycle calls also emit local
`telemetry` category events with additive `trace_id` and `span_id` fields.
The span captures its session at start and children inherit the explicit
parent's captured session, so completion after a TUI session switch remains
correlated with the original session, locally and in OTLP. Captured `session.id`,
`run.id` and `mode` are exported; `session.name` requires
`observability.include_session_name = true` and persisted session identity.
ACP's ephemeral session identifier is not exported as persisted identity.
`observability.include_tool_details = true` additionally permits `shell.command`
and whitelisted scalar `tool.target` attributes, using the same native redactor
and 128-byte bound in both sinks. Default-off, this never exports bulk arguments,
environment, stdin or output; existing free-form log policy is unchanged.
See [tool detail policy](observability.md#opt-in-tool-details).

The local structured adapter decodes the already filtered/redacted native
LogRecord attributes and persists an additive `attributes` array of `{key,value}`
pairs, preserving strings, numbers, booleans and repeated keys in order. It does
not reinterpret raw messages or define a second attribute policy. The lifecycle
local-write adapter is synchronous; write failure does not prevent native span
completion or export. Existing runtime content, redaction, rotation, `/logs` and
`capstan.log.v1` remain compatible; the compact `/logs` view is not a structured
attribute browser. Local lifecycle logging is independent of OTLP enablement.
Strict isolated states still receive neither contexts nor either sink; isolation
is sticky, including saved closures.

Exporter polling/shutdown never append local diagnostics or print stderr.
`capstan.telemetry.diagnostics()` returns an in-memory snapshot of loss/error
counters and queue/enabled state, including shutdown losses. Each permitted
explicit pull also attempts a synchronous `exporter.diagnostics` local event
with numeric/boolean attributes in the current scope, without trace/span IDs.
Consequently this API can perform log I/O; it is not nonblocking. A recursion
guard prevents nested writes; log failure leaves the snapshot/counters unchanged.
Isolated states receive `nil` and do not log a snapshot. There is no autonomous
asynchronous diagnostic-queue adapter. With both sinks disabled the startup slot
is discarded. See
[Config](config.md#native-opentelemetry) for exact environment precedence and
[Observability](observability.md#diagnostics) for counter semantics.

## Architecture

`src/log.c` exposes `capstan.log(category, message)`, `capstan.log_path()`, and
a bounded allowlisted tail reader to Lua. Lock acquisition returns the exact
daily path covered by the lock; the tail reader accepts only that log and its
JSONL/legacy archive names, rejects symlinks, and requires the shared log lock.
The logger appends to the current daily log, rotates
over-size files before opening them, and creates the Capstan state directory if
needed. Runtime log redaction calls `agent.redact.text()` when the
Lua runtime is available, so config-driven rules are applied consistently to
Lua-visible output and C-originated runtime log messages. `src/redact.c` is only
an emergency fallback for early logging or Lua redactor failure; it intentionally
contains a smaller built-in rule set and fails closed with `[REDACTION_FAILED]`
instead of returning unredacted input.

The `agent/` Lua runtime writes agent/tool/API lifecycle events through that Lua
API.
`plugins/logs.lua` reads the log file and returns a tail view.
`session_manager.c` changes the logger scope after TUI session creation or a
successful switch. Headless CLI setup selects its explicit scope before plugin
initialization so provider, tool, permission, and hook events share one file.

## Tests

`make test-http-lua` covers provider-level log calls, scoped paths and
permissions, TUI scope switching, the `/logs` plugin, and structured attribute
serialization (types, duplicate keys, escaping and invalid numeric rejection).
Numeric JSON attributes always use a dot radix regardless of `LC_NUMERIC`;
`test/test_jsonl.c` checks fractional/exponent values in available C and comma
locales without changing the caller's locale.
`make test-telemetry` independently decodes OTLP and checks the native local
adapter, captured context, redaction, isolation and explicit diagnostics pulls.
`make test-telemetry-modes` covers real-binary CLI/TUI/ACP context and lifecycle
export to a loopback receiver, not a production backend UI.
`make test-build` verifies `/logs` is embedded in the standalone binary.
