# Subagents

Subagents let the orchestrator split work into independent internal agent runs.
The public model tool is `subagents`: one call may contain several tasks, and
Capstan runs up to the configured concurrency limit at the same time.

## Behavior

Subagents are enabled by default. Disable explicitly:

```lua
return {
  capabilities = {
    subagents = false,
  },
}
```

Configuration defaults:

```lua
return {
  subagents = {
    max_concurrent = 3,
    max_tasks = 8,
    max_attempts = 3,
    max_turns = 6,
    max_turns_cap = 200,
  },
}
```

Model tool shape:

```json
{
  "tasks": [
    {
      "id": "docs",
      "task": "Study documentation and summarize relevant constraints",
      "model": "deepseek-chat",
      "max_turns": 6,
      "tools": ["file_read", "fetch"]
    }
  ],
  "max_concurrent": 3
}
```

`tasks` is required. Model/max_turns/tools are optional per task. Subagents
always use the orchestrator's current provider. Before launching, Capstan
fetches that provider's model list; a task `model` is used only when it appears
in that current-provider list, otherwise the current active model is used.
`tools` only narrows the parent tool list; it cannot add tools unavailable to
the orchestrator.

Subagents do not have a separate permission pattern. The user controls exposure
with `capabilities.subagents` and controls scale with the `subagents.max_*`
limits above. The tools used inside subagents still go through their own normal
permissions.

Each task may be attempted up to `subagents.max_attempts` times. Automatic
retry is reserved for transient transport/API failures: connection errors and
HTTP `408`, `429`, `500`, `502`, `503`, or `504`. Client/auth/request-shape
errors such as `400`, `401`, and `403` are not retried automatically; they
require an orchestrator repair/analyze step before another request would be
useful.

## Runtime Contract

- Subagents run in-process as independent `capstan.agent.run` executions.
- Network waits are parallel through `curl_multi`; Lua callbacks and tool
  handlers still execute on the single Lua thread.
- Max depth is `1`: subagents do not receive the `subagents` tool.
- Subagent text and inner tool rows do not stream into the parent message.
- Each subagent start is written to runtime logs with task id, selected model,
  current provider, and compact prompt preview.
- The parent receives one JSON tool result and must synthesize the final answer.

Result shape:

```json
{
  "ok": true,
  "duration_ms": 18420,
  "total_turns": 6,
  "results": [
    {
      "id": "docs",
      "ok": true,
      "text": "...",
      "error": "",
      "turns": 2,
      "attempts": 1,
      "started_at": 1720000000120,
      "finished_at": 1720000008240,
      "duration_ms": 8120
    }
  ]
}
```

Results are emitted in input task order for stable `id` matching. Execution and
completion are parallel and may occur in a different order.

## UI

The parent message shows a compact status block:

```text
⚙ subagents: running 3 concurrent, 3 total
  docs - Study documentation and summarize relevant constraints
  build - Check build scripts and likely failure points

⚙ subagents: done 2/3, error 0/3, 8.1s
  docs - done, 2 turns, 8.1s
  build - done, 1 turn, 4.2s
```

## Hooks

`after_subagents` runs after all internal runs complete and before the JSON tool
result is returned. It receives `ctx.args`, `ctx.ok`, and `ctx.result`.

## Tests

`make test-http-lua` covers default exposure, capability disablement, stream
error propagation, and provider/tool runtime behavior.
