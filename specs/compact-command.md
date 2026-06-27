# Compact Command

## Behavior

`/compact` summarizes the current conversation into a compact operational
handoff and replaces the visible message history with that summary.

- The command is built into the dispatcher, not loaded as a Lua plugin, because
  it must replace the C-owned message history.
- Empty agent placeholders are not sent to the compact model.
- The compacted result is stored as a synthetic user message whose raw content
  tells the next model to continue from the compacted state.
- The command does not add the literal `/compact` input to history and does not
  run the normal agent turn.
- If no conversation exists, Capstan shows a popup and does not call the model.

## Model Selection

`/compact` prefers `capstan.models.weak()` when a weak provider/model has been
configured through `/models --weak` or `config.lua`.

If no weak model is configured, compacting falls back to the active primary
provider/model. Compact requests pass `update_status = false` and
`update_usage = false`, so fallback does not change the visible provider/model
status line or token counters.

The compact run disables model tools with `tools = {}` and uses `max_turns = 1`.

## Architecture

C owns the message array, so `src/agent.c` exposes:

- `agent_compact(lua_State *L)`: builds a Lua copy of current non-empty
  messages and calls `_G.compact_entry(messages)`.
- `agent.replace_compacted_context(summary)`: Lua callback that clears C
  history and inserts the compacted handoff message.

Lua owns provider policy, so `agent/runtime.lua` builds the compact prompt,
selects the weak model, runs the normal `capstan.agent.run` path, and calls the
C replacement callback on completion.

## Tests

`make test-http-lua` covers the C history replacement callback and the Lua
compact entrypoint. `make test-build` verifies embedded runtime assets still
load in the standalone binary.
