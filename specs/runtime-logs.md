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

Log files rotate daily by filename. If the current day's file reaches 10 MiB
before the day changes, Capstan renames it to `.1.log` and keeps up to five
same-day archives:

```text
logs/2026-06-27.log
logs/2026-06-27.1.log
logs/2026-06-27.2.log
```

## Logged Events

- Agent request start: provider, model, message count, tool count
- Tool names sent to the model
- Last outbound message role and compact content preview
- API stream request: endpoint, message count, tool count
- Stream summary: SSE event count, raw byte count, chunk counters, final tool
  call count, text/reasoning byte counts
- Tool-call stream deltas: index, id, accumulated name, accumulated argument
  byte count
- Final reconstructed tool calls with id, name, and compact arguments
- Assistant text when the stream completes without tool calls
- Tool calls received from the model
- Tool call name, permission target, and raw JSON arguments
- Shell tool calls log a redacted `display` label and redacted JSON arguments.
  Curl commands are summarized as `curl <url>` when possible. Raw shell command
  strings are not rendered in the conversation status.
- Subagent starts: task index, task id, current provider, selected model, and a
  compact prompt preview.
- [Permission](permissions.md) checks and prompt decisions
- Tool completion and result size
- Tool handler failures, including compact diagnostic text
- Tool guard stops under the `tool_guard` category when the runtime aborts a
  runaway loop before the next tool execution
- Plugin load/reload failures from `~/.config/capstan/plugins/*.lua`
- Continuation after tool results
- [Hook](hooks.md) errors with stage and source

Set `CAPSTAN_LOG_RAW=1` to include raw SSE chunks and parsed SSE event payloads
in the log. This is intentionally opt-in because raw stream logs can become
large and may include full model output.

## Architecture

`src/log.c` exposes `capstan.log(category, message)` and
`capstan.log_path()` to Lua. The logger appends to the current daily log,
rotates over-size files before opening them, and creates the Capstan state
directory if needed.

The `agent/` Lua runtime writes agent/tool/API lifecycle events through that Lua
API.
`plugins/logs.lua` reads the log file and returns a tail view.

## Tests

`make test-http-lua` covers provider-level log calls and the `/logs` plugin.
`make test-build` verifies `/logs` is embedded in the standalone binary.
