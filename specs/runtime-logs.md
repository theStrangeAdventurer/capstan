# Runtime Logs

## Behavior

Capstan writes runtime events to:

```text
$XDG_STATE_HOME/capstan/logs/YYYY-MM-DD.log
```

When `XDG_STATE_HOME` is not set, the fallback is
`~/.local/state/capstan/logs/YYYY-MM-DD.log`.

Each line includes a local timestamp, category, and compact message:

```text
2026-06-19 18:30:00 [tool] call name=shell target=/Users/me/project display=~/project args={"command":"shell"}
```

The `/logs [n]` command displays the last `n` log lines in the conversation.
The same plugin exposes a `logs` model tool so the agent can inspect recent
runtime events when debugging failed tools, plugins, hooks, or API calls. If
`n` or `limit` is omitted, it shows the last 80 lines. The maximum is 500 lines.

Runtime logging honors `LOG_LEVEL`. Supported values are `error`, `warn`,
`info`, `debug`, and `trace`. The default is `info`, which keeps high-signal
lifecycle, tool, permission, and error events. `debug` adds low-level
stream/tool-call reconstruction events. `trace` also enables raw SSE/event
payload logging.

Log files rotate daily by filename. If the current day's file reaches 10 MiB
before the day changes, Capstan renames it to `.1.log` and keeps up to five
same-day archives:

```text
logs/2026-06-27.log
logs/2026-06-27.1.log
logs/2026-06-27.2.log
```

Before a message is written, the logger redacts common secret shapes through the
canonical Lua redactor in `agent/redact.lua`: sensitive HTTP headers,
token/password key-value pairs, environment variables whose names contain
credential markers, and additive `redaction` rules from
`~/.config/capstan/config.lua`; see [Config](config.md). Built-in redaction is
not disabled by config.

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
- Tool handler failures, including compact diagnostic text
- Tool guard stops under the `tool_guard` category when the runtime aborts a
  runaway loop before the next tool execution
- Plugin load/reload failures from `~/.config/capstan/plugins/*.lua`
- Continuation after tool results
- [Hook](hooks.md) errors with stage and source

Set `LOG_LEVEL=trace` to include raw SSE chunks and parsed SSE event payloads
in the log. This is intentionally opt-in because raw stream logs can become
large and may include full model output. Raw stream logs still pass through the
best-effort redactor, but this mode should be treated as sensitive debug output.

## Architecture

`src/log.c` exposes `capstan.log(category, message)` and
`capstan.log_path()` to Lua. The logger appends to the current daily log,
rotates over-size files before opening them, and creates the Capstan state
directory if needed. Runtime log redaction calls `agent.redact.text()` when the
Lua runtime is available, so config-driven rules are applied consistently to
Lua-visible output and C-originated runtime log messages. `src/redact.c` is only
an emergency fallback for early logging or Lua redactor failure; it intentionally
contains a smaller built-in rule set and fails closed with `[REDACTION_FAILED]`
instead of returning unredacted input.

The `agent/` Lua runtime writes agent/tool/API lifecycle events through that Lua
API.
`plugins/logs.lua` reads the log file and returns a tail view.

## Tests

`make test-http-lua` covers provider-level log calls and the `/logs` plugin.
`make test-build` verifies `/logs` is embedded in the standalone binary.
