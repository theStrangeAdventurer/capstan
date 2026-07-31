# Agent Control

Capstan supports lightweight controls for interrupting the current agent output
and clearing conversational context.

## Behavior

- Pressing Esc in input mode while an async agent response is streaming cancels active
  stream transfers.
- Cancellation stops future chunks, clears the thinking indicator, and appends
  `[stopped]` to the current agent message.
- Space always keeps its normal input behavior.
- `/new` saves the current conversation and creates a new empty session.
- `/new` clears pending context badges and resets token usage and thinking state.
- `/sessions` lists and restores conversations saved for the active workspace.

## Architecture

`http_cancel_streams()` removes active curl multi handles, unreferences Lua
callbacks, and frees stream resources.

The main input loop handles Esc in input mode before regular text insertion and
calls the stream cancellation helper when `http_is_loading()` is true. `/new`
and `/sessions` are built-in dispatch commands because they need direct access
to persisted sessions, message storage, pending context storage, usage state,
and active stream cancellation.

## Constraints

- Blocking plugin HTTP calls still run inside Lua plugin execution and cannot be
  interrupted by Esc until control returns to the main loop.
- Session switching follows the queued-input policy and is unavailable until the
  active top-level run completes.

## Test Notes

Existing build and smoke tests cover compilation and command registration. The
stream cancel path is primarily verified by the curl-backed build because it
depends on libcurl multi state.
