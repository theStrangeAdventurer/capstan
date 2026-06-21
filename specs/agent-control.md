# Agent Control

Capstan supports lightweight controls for interrupting the current agent output
and clearing conversational context.

## Behavior

- Pressing Space while an async agent response is streaming cancels active
  stream transfers.
- Cancellation stops future chunks, clears the thinking indicator, and appends
  `[stopped]` to the current agent message.
- Space keeps its normal input behavior when no HTTP stream is active.
- `/new` clears all visible messages and pending context badges.
- `/new` resets token usage and thinking state.
- `/new` does not create a persisted session; it only clears the current
  in-memory context.

## Architecture

`http_cancel_streams()` removes active curl multi handles, unreferences Lua
callbacks, and frees stream resources.

The main input loop handles Space before regular text insertion when
`http_is_loading()` is true. `/new` is a built-in dispatch command because it
needs direct access to message storage, pending context storage, usage state,
and active stream cancellation.

## Constraints

- Blocking plugin HTTP calls still run inside Lua plugin execution and cannot be
  interrupted by Space until control returns to the main loop.
- `/new` is intentionally in-memory only until a session model exists.

## Test Notes

Existing build and smoke tests cover compilation and command registration. The
stream cancel path is primarily verified by the curl-backed build because it
depends on libcurl multi state.
