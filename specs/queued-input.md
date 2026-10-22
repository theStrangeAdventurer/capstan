# Queued Input

Capstan keeps the input editor usable while an interactive agent run is active.
Submitting ordinary text during that run queues it instead of starting a second
run.

## Behavior

- The queue is FIFO and holds at most five non-empty messages.
- Up to three queued messages are shown as pinned one-line previews above the
  input editor. Each preview is clipped to the available width; additional
  queued messages remain in FIFO order but do not consume screen rows.
- Previews use neutral secondary gray rather than warning yellow.
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
  Commands executed from autocomplete also clear the submitted command before
  invoking the handler, preserving any fresh draft entered during its waits.
- Synchronous HTTP waits, including the `Delegating` subagent wait loop,
  service the same blocking TUI input pump instead of only repainting. The pump
  does not poll HTTP or execute new agent runs recursively; headless waits never
  touch terminal input.
- Bracketed paste uses one incremental body decoder in both the main loop and
  the blocking pump. UTF-8 bytes and newlines stay in the draft; pasted shortcuts
  and slash commands are text, not actions. Partial end markers survive idle
  frames and the return from a blocking wait. There is no body timeout that could
  silently turn the rest of a delayed paste into commands. Input processing in
  the blocking pump is bounded to 256 keys per frame so pastes do not starve work.
- Slash commands are not queued. While a run is active or queued submissions
  are pending, they remain in the input editor and Capstan reports that commands
  are unavailable. The same guard applies to Enter, Tab/automatic command menus,
  and confirmation of an already-open selection (including session switching
  and directory drill-down). Rejected selections do not clear the draft or call
  plugin handlers/autocomplete fetchers.
- Esc cancellation finishes the active run after cancelling its streams, then
  allows the queued batch to start from the main loop.
- The queue is in-memory only and is cleared when Capstan exits.

## Collapsed pasted text

- A completed bracketed paste occupying more than two visible input rows is
  displayed inline as `[Pasted N chars]`. N counts Unicode codepoints (including
  newlines), not bytes or terminal cells. Short pastes remain ordinary text.
- The threshold measures the pasted payload alone, starting at column zero,
  using explicit newlines and Unicode terminal-cell wrapping at the content
  width captured at paste begin (terminal width minus margins and borders).
  Exactly two rows remain expanded; a trailing newline adds a row. Resizing
  does not reclassify completed blocks. The non-TUI default width is 80 cells.
- The paste appears progressively as raw text until its complete end marker
  arrives. Existing CR-to-LF normalization and literal partial-marker handling
  remain unchanged.
- Blocks keep their inline positions among ordinary text, other blocks, and
  image prefixes. Left/right traverse a block atomically; Backspace immediately
  after one deletes its entire payload. Word/line deletion touching any part of
  a block removes that whole block rather than leaving a partial hidden range.
  Ctrl+U immediately after a block also deletes it when its payload ends in a
  newline; an ordinary uncollapsed trailing newline retains existing behavior.
- Clear, initialization, and replacement of the draft discard block metadata.
  The original normalized bytes remain the canonical input for submission,
  queued messages, and model context. Paste labels are input-editor-only: chat
  history and saved messages contain the expanded text, retaining image labels
  when images are attached.
- The existing draft limit remains **8191 bytes** plus its NUL terminator.
  Excess paste bytes are ignored as before, but the end marker is still consumed.
  Labels count only retained content. Metadata allocation failure leaves the
  paste expanded with no additional data loss. Display storage covers worst-case
  label expansion independently of the fixed raw-input capacity.

## Architecture

C owns queue storage and dispatch timing. Lua calls `agent.finish_run()` from the
TUI adapter's top-level `on_done` callback. That callback only marks the run as
finished; `dispatch_tick()` starts a queued batch later from the main event loop,
never recursively from an HTTP callback.

## Tests

`make test` covers queue capacity, FIFO order, empty input rejection, extraction,
and cleanup. `make test-http-lua` and `make test-build` cover the C/Lua bridge and
embedded runtime build. `make test-tui-input` runs the real ncurses binary in
an isolated pseudo-terminal with a local wait fixture (no API calls). It checks
live dictation-style UTF-8/multiline paste during waiting, FIFO submission without
recursive dispatch, paste spanning the return to the main loop, manual-command
draft retention, ordinary idle paste, and rejection of Tab/Enter commands during
an asynchronous run with the draft still usable after cancellation. Unit tests
cover the shared command guard for active runs and pending queues, split/literal end
markers, full input buffers, and HTTP wait frames servicing input only in TUI.
