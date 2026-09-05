# Editor Command

`/editor` opens the current prompt draft in the user's system editor, then
returns the edited text to the input box without submitting it to the LLM.

## Behavior

| Input | Action |
|-------|--------|
| `/editor` | Open an empty temporary prompt file in the editor |
| `/editor <text>` | Open a temporary prompt file prefilled with `<text>` |
| `/` command picker | Show `/editor  Edit prompt in $EDITOR` with plugin commands |

After the editor exits successfully, the temporary file contents replace the
input buffer. The user must press Enter again to submit the edited prompt.

Only complete text up to `INPUT_BUFFER_SIZE - 1` bytes (currently 8191) is
imported, including an exact-limit draft. The limit counts UTF-8 bytes, not
characters. Larger files are never partially imported. Embedded NUL bytes are
also rejected rather than silently truncating the draft.

On overflow, open/read failure, or unsuccessful editor exit, the temporary file
is left untouched for recovery. The input buffer shows the specific cause
(byte limit, OS error, exit code, or signal) and the temporary file path.
Successful imports replace the draft and remove the temporary file. These local
errors and drafts are not automatically sent to the model or persisted as chat.

## Editor selection

Editor lookup order:

1. `$EDITOR`
2. `$editor`

The lowercase fallback exists because the feature is intended to follow the
user's configured global editor variable even if their environment uses a
non-standard lowercase name.

## Architecture

`/editor` is implemented as a C builtin command instead of a Lua plugin.

Reasons:

- It changes TUI/input state rather than producing a user message or LLM
  context item.
- It must preserve the input buffer after Enter instead of following the normal
  dispatch path that clears input.
- It temporarily suspends ncurses while the external editor owns the terminal.
- The current Lua plugin API has no `input.set_text`, `tui.suspend`, or
  `tui.resume` primitives.

Implementation points:

- `src/dispatch.c` detects `/editor` before plugin lookup.
- `src/editor.c` writes the initial draft to a `mkstemp` file.
- `src/editor.c` runs the editor with ncurses suspended:

```c
def_prog_mode();
endwin();
system(command);
reset_prog_mode();
refresh();
```

- On success, the edited file is read back into `input_set_text`.
- The command returns a special dispatch status so `dispatch_submit()` does not
  call `input_clear()`.

## Non-goals

- `/editor` does not auto-submit the prompt after editing.
- `/editor` does not expose a general Lua API for terminal suspension.
- `/editor` does not preserve the original `/editor` command text after a
  failed editor run; failures are surfaced in the input buffer.

## Testing Notes

`src/editor_prompt.c` owns bounded, transactional file reading without curses.
It leaves the destination unchanged on failure and never modifies/removes the
file; `src/editor.c` owns UI diagnostics and successful-import cleanup.
`test/test_editor.c` covers exact-limit input, UTF-8 overflow, unchanged destination
and complete retained file, empty input, NUL rejection, open failure and read
failure. Interactive smoke testing should additionally check failed editor exits
and recovery-path diagnostics; process control and ncurses remain in the adapter.
