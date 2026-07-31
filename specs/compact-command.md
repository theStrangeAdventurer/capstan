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
- Compacting counts as an active top-level operation. Ordinary submissions use
  the existing bounded FIFO and are dispatched after successful replacement;
  slash commands remain unavailable until compacting finishes.
- If no conversation exists, Capstan shows a popup and does not call the model.
- Provider errors and empty or whitespace-only summaries preserve the original
  history. Compacting does not invoke `after_agent_turn` hooks.

## Model Selection

`/compact` prefers `capstan.models.weak()` when a weak provider/model has been
configured through `/models --weak` or `config.lua`.
Profile-specific models configured through `/models --profile ...` or
`agent.profile_models` do not affect compacting.

If no weak model is configured, compacting falls back to the active primary
provider/model. Compact requests pass `update_status = false` and
`update_usage = false`, so fallback does not change the visible provider/model
status line or token counters.

The compact run disables model tools with `tools = {}` and uses `max_turns = 1`.
When the weak model has a known context limit, Capstan first estimates the
complete compact request against that limit. If it would consume 90% or more,
the weak model is skipped and the active primary model performs the summary.
For manual `/compact`, an unknown weak-model limit preserves the configured
weak-model behavior. Automatic compaction fails safe: it skips a weak model
whose capacity cannot be verified and uses the active primary model whose
known limit caused the threshold decision.

## Automatic Compaction

Before an ordinary interactive submission, Capstan estimates the next complete
model request. The estimate includes system and profile instructions, existing
history, the pending user text, and the currently available model-tool schemas.
Text uses a script-aware UTF-8 estimate: ASCII keeps the common four-byte
approximation, two-byte code points are charged more heavily, and CJK/emoji
code points count approximately one token each. The configured threshold still
leaves provider-tokenizer and hook expansion headroom; it is not presented as
an exact provider token count.
If it reaches `agent.auto_compact_percent` (80 by default), the dispatcher:

1. moves buffered plugin context into the existing history;
2. keeps the new user text in the normal bounded FIFO;
3. compacts the existing history;
4. dispatches the queued text against the compacted handoff.

The queued text is deliberately excluded from the summary so the primary model
still answers the user's exact submission. A failed or canceled compact keeps
the original history and dispatches the queued text against it. The automatic
path runs at most once for a submission; the post-compact dispatch does not
perform another threshold check. Set `agent.auto_compact_percent = 0` to
disable the feature. If the active model's context limit is unknown, Capstan
fails open and sends the normal request without automatic compaction.

## Architecture

C owns the message array, so `src/agent.c` exposes:

- `agent_compact(lua_State *L)` and `agent_auto_compact(lua_State *L)`: build a
  Lua copy of current non-empty messages and call
  `_G.compact_entry(messages, automatic)`.
- `agent.replace_compacted_context(summary)`: Lua callback that clears C
  history and inserts the compacted handoff message.

Lua owns provider policy, so `agent/runtime.lua` builds the compact prompt,
selects the weak model, runs the normal `capstan.agent.run` path, and calls the
C replacement callback on completion.

## Tests

`make test-http-lua` covers history replacement, busy-state release, provider
errors, whitespace summaries, hook suppression, automatic threshold decisions,
non-ASCII estimates, and weak-model context fallback. `make test-build`
verifies embedded runtime assets still load in the standalone binary.
