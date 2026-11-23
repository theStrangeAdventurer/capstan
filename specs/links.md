# Links in the transcript

Web URLs and local file paths in the message pane are underlined and open with a
modifier click. Underlining is always on, because a terminal cannot report that
Shift is being held without a key event.

## Behavior

- Web links (`http://`, `https://`) and path-looking tokens are underlined in the
  transcript. A trailing sentence dot, comma, semicolon, colon or bang is not part
  of the link.
- Code is not a link. Inside inline code, fenced code, tool-status lines, tagged
  shell output and diffs (every byte the projection marks `MARKDOWN_LITERAL`) a
  path is neither underlined nor clickable, because it is sample text, not a
  target. Web links are the one exception: an `http`/`https` URL shown in code
  stays underlined and still opens.
- Paths are recognised when the token contains `/` and is absolute (`/usr/bin/x`),
  home-relative (`~/notes/a.md`), explicitly relative (`./a`, `../a`) or has an
  extension in its last component (`src/tui.c`). Prose such as `и/или` and `24/7`
  is not a path, and a URL is never treated as a path.
- Opening a link needs no modifier: a click that does not turn into a drag opens
  the span under the cursor, so a link works the same way as in a browser. A drag
  still selects text, and plain clicks elsewhere keep their old meaning.
- Shift+click and Ctrl+click open the span on the press, for terminals where the
  modifier reaches the application and for users who expect it.
- Web links open in the default browser; file paths are revealed in the file
  manager (the containing directory on Linux). A relative path is tried against
  the working directory first and then against the workspace root, so transcript
  paths that were written relative to the workspace still resolve.
- Relative paths resolve against the working directory; `~` resolves through
  `$HOME`. A path that does not exist, or a target that is not a web link, shows
  a short error popup instead of running anything.
- Wrapping is transparent: link spans are found in the whole projected message,
  so a link that wraps over several rows resolves to the same span from any of
  its rows.
- Successful opens are not confirmed with a popup: the browser or file manager is
  the feedback, and a transient popup would swallow the next click.
- Only `http`/`https` are opened. Any other scheme is refused before a command is
  spawned.

## Architecture

`src/links.c` owns the whole policy: span scanning, hit testing, the
code-span rule (`links_allowed`: paths are inert when the byte is literal, web
links never are), path resolution with the workdir/workspace fallback,
validation and launching. It runs openers without a shell
(`execvp` with a literal argv) and without leaving a zombie (double fork, the
intermediate child exits immediately).

`src/tui.c` scans each message projection once per view rebuild
(`LINK_SPAN_LIMIT` spans per message), applies `A_UNDERLINE` while writing the
visible runs, and resolves a click through the line map: screen cell to
character to byte offset inside the projected text. `src/main.c` routes
Shift/Ctrl clicks before the selection handlers, so a modifier click never starts
a drag.

`src/popup.c` no longer keeps its own URL launcher: selecting a link inside a
message popup and clicking it goes through `links_open_text`, so one owner opens
targets for both the transcript and the popup. A click outside a transient toast
now dismisses it and keeps its meaning instead of being swallowed.

## Constraints

- The code-span rule is per byte from the projection style flags, so a link that
  starts outside a code span and continues inside it is only actionable in its
  non-code part.
- Image path lines are a separate feature and still render inside fenced code; only
  links use the literal rule.

- Bare Shift cannot be observed, so links are underlined permanently rather than
  only while a modifier is held.
- A path containing spaces is split at the space and will not open; the same
  applies to links wrapped in brackets other than the trimmed ones.
- Only the visible projection is scanned: the opened value is exactly the text on
  screen, never a hidden destination from Markdown source.
- File-manager semantics differ per platform: `open -R` reveals a file in Finder,
  directories open directly; on Linux `xdg-open` receives the containing folder.

## Tests

`make test` covers the code-span rule (`links_allowed`), URL and path scanning, sentence punctuation trimming, rejection
of prose and non-web schemes, offset hit testing, and a link wrapped over several
rows resolving to one span.

`make test-tui-input` covers the rendered result: `links` asserts the underline
attribute and that a Shift+click on a URL runs the opener with that URL;
`links_path` asserts the same for a relative path that resolves to an existing
file and is revealed through the opener. `links_plain` covers the modifier-free
click: the click opens the path and the release must not start a drag. This PTY
drops mouse release events, so that case reports itself as skipped rather than
passing silently when the release never arrives. `links_code` appends inline and fenced
code containing a path and a URL: both URLs are underlined, the code paths are
not, and a Shift+click on a code path opens nothing. Each case performs a single click,
because a PTY without SGR mouse reporting (the fixture runs `TERM=xterm-256color`)
drops the mouse press that follows a click.
