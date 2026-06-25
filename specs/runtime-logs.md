# Runtime Logs

## Behavior

Capstan writes runtime events to:

```text
$XDG_STATE_HOME/capstan/events.log
```

When `XDG_STATE_HOME` is not set, the fallback is
`~/.local/state/capstan/events.log`.

Each line includes a local timestamp, category, and compact message:

```text
2026-06-19 18:30:00 [tool] call name=shell target=/Users/me/project display=~/project command=make test args={"command":"make test"}
```

The `/logs [n]` command displays the last `n` log lines in the conversation.
If `n` is omitted, it shows the last 80 lines. The maximum is 500 lines.

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
- Shell tool calls additionally log a `display` directory and separate
  `command`. In the conversation, shell status renders the directory on the
  first line and the executed command on a quieter second line.
- [Permission](permissions.md) checks and prompt decisions
- Tool completion and result size
- Continuation after tool results
- [Hook](hooks.md) errors with stage and source

Set `CAPSTAN_LOG_RAW=1` to include raw SSE chunks and parsed SSE event payloads
in the log. This is intentionally opt-in because raw stream logs can become
large and may include full model output.

## Architecture

`src/log.c` exposes `capstan.log(category, message)` and
`capstan.log_path()` to Lua. The logger appends to `events.log` and creates the
Capstan state directory if needed.

The `agent/` Lua runtime writes agent/tool/API lifecycle events through that Lua
API.
`plugins/logs.lua` reads the log file and returns a tail view.

## Tests

`make test-http-lua` covers provider-level log calls and the `/logs` plugin.
`make test-build` verifies `/logs` is embedded in the standalone binary.
