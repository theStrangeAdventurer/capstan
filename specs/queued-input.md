# Queued Input

Capstan keeps the input editor usable while an interactive agent run is active.
Submitting ordinary text during that run queues it instead of starting a second
run.

## Behavior

- The queue is FIFO and holds at most five non-empty messages.
- Up to three queued messages are shown as pinned one-line previews above the
  input editor. Each preview is clipped to the available width; additional
  queued messages remain in FIFO order but do not consume screen rows.
- Each preview row is cleared before repainting, so a shorter item cannot leave
  stale characters from a longer item previously drawn on the same row.
- A full queue leaves the sixth message in the editor and reports that the
  queue is full.
- Queued messages are not added to conversation history until the active run
  finishes.
- When the run finishes, all queued messages are added as separate consecutive
  user messages, followed by one assistant placeholder and one agent run.
- Messages entered while that batch runs form the next queue.
- The blocking TUI pump accepts typing in the editor, but Enter submits only
  while a top-level agent run is active, where submission is guaranteed to
  enqueue. During blocking plugin, MCP, shell, or permission work outside an
  agent run, Enter leaves the draft intact for submission after the blocking
  operation returns; it never starts a nested dispatch on the same Lua state.
- Slash commands are not queued. While a run is active they remain in the input
  editor and Capstan reports that commands are unavailable.
- Esc cancellation finishes the active run after cancelling its streams, then
  allows the queued batch to start from the main loop.
- The queue is in-memory only and is cleared when Capstan exits.

## Architecture

C owns queue storage and dispatch timing. Lua calls `agent.finish_run()` from the
TUI adapter's top-level `on_done` callback. That callback only marks the run as
finished; `dispatch_tick()` starts a queued batch later from the main event loop,
never recursively from an HTTP callback.

## Tests

`make test` covers queue capacity, FIFO order, empty input rejection, extraction,
and cleanup. `make test-http-lua` and `make test-build` cover the C/Lua bridge and
embedded runtime build.
