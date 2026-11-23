# Terminal images

A path to a local image in the conversation is rendered as the image itself,
below the path line, in terminals that support the Kitty graphics protocol.

## Behavior

- The message pane detects it when the whole displayed line is a path to an
  existing local image file: PNG, JPEG, GIF or WebP. Detection uses the file
  signature, not just the extension.
- Absolute and relative paths both work; relative paths resolve against the
  working directory. Surrounding quotes, backticks, brackets and trailing
  `,`/`.`/`:` are ignored, and Markdown list markers (`- `, `* `, `1. `) and
  block-quote prefixes (`> `) are skipped.
- A path inside a sentence is not an image line, and neither is a line that
  wraps in the pane: the reservation needs the whole path on one displayed row.
- The image is placed directly below its path line. The path line stays visible
  and selectable.
- The band is `rows` reserved lines in the line map, so text below an image keeps
  its position while the band is above the viewport: reserved rows advance the
  screen row only while they are actually visible, exactly like text rows.
- A band is placed only when its first row is visible and the whole band fits the
  pane. A band that cannot be placed shows one dim caption in its first visible
  row (`▣ shot.png 640x160`) instead of an unexplained gap; the caption is
  renderer-only and never enters copied text, model context or persistence.
- Band height is capped at the pane height minus the anchor line, the two spacer
  rows and the trailing padding row, so the last image in a conversation is always reachable by
  scrolling to the bottom.
- Rendered width is clamped to 320–640 px and can never exceed the pane. Height
  follows the source aspect ratio, and a tall image shrinks so its band fits the
  message pane instead of overflowing it (a square 640 px image in a 20-row pane
  becomes 40x20 cells). Terminal cell pixel size comes from
  `TIOCGWINSZ` when the terminal reports it (`8x16` otherwise).
- The protocol is Kitty graphics (base64, 4096-byte chunks, `c`/`r` sized in
  cells so the placement matches the reserved rows). Kitty, Ghostty, WezTerm and
  iTerm2 3.6+ implement it.
- Detection is environment based: `KITTY_WINDOW_ID`, `WEZTERM_EXECUTABLE`,
  `GHOSTTY_RESOURCES_DIR`, `TERM`/`TERM_PROGRAM` containing `kitty`, `ghostty`
  or `wezterm`, or `TERM_PROGRAM` containing `iterm` with version 3.6 or newer.
  `CAPSTAN_TERMINAL_IMAGES=off` disables rendering, `=on` forces it. Environments
  that report `TMUX` or `ZELLIJ` stay disabled unless forced, because those
  multiplexers do not keep graphics pane-local on their own.
- Degradation: unsupported terminals keep the plain path text and no rows are
  reserved, so nothing is lost and no empty gap appears. `CAPSTAN_TERMINAL_IMAGES=off`
  forces that mode; the next frame also deletes any placement already on screen.
- Clicking a placed image copies its pixels to the system clipboard. The only
  feedback is the usual short popup, in English like the other popups:
  `Image copied` for about two seconds. The band does not flash and the picture
  is not cleared, so it stays visible while the confirmation appears. When the
  platform cannot place that format on the clipboard, the file path is copied
  instead (`Path copied`); a total failure reports `Copy failed`.
- Two spacer rows are reserved between the path line and the picture, and one
  caption row sits below it, so nothing is glued together: path text, gap,
  picture, caption row.
- Up to 8 images are rendered per frame. The placement and file caches mean the
  payload is re-sent only when the visible set or its positions change.

## Architecture

`src/terminal_image.c` owns the whole policy: environment detection, whole-line
path detection, header-derived pixel dimensions, cell geometry and the escape
output. It depends on ncurses, Lua and curl only through
`clipboard_base64_encode`, so it is unit-tested directly.

`src/linemap.c` reserves the band: a per-line callback
(`linemap_set_image_rows`) returns the image height in rows, and the line map
stores that many extra `LINE_IMAGE` entries after the anchor line. Because the
reservation lives in the line map, scrolling, selection, copy, and mouse
hit-testing keep a single source of truth for rows to lines.

`src/tui.c` renders the message pane from those entries: it clears each band
row, queues a placement for the first row of the band, and writes the escape
output right after `doupdate()`. Cursor position is saved and restored around
each placement, so ncurses' own cursor tracking stays valid. Reserved rows are
negative roles, which `src/visual.c` already treats as non-text: the band cannot
be selected or copied.

`src/clipboard.c` owns the platform clipboard commands:
`clipboard_copy_image(path, mime)` uses `osascript` with the matching AppleScript
image class on macOS and `wl-copy --type`/`xclip -t` on Linux.
`terminal_image_mime` provides the signature-checked MIME type, so the class or
type is never guessed from the file name. `tui_handle_image_mouse` matches the
click against the placements collected for the current frame and runs on the
press, before a press could start a selection drag.

Image bytes never enter `Message.text`, model context, logs, session files or
token counting; only the display projection and the terminal output are
affected.

## Constraints

- Kitty graphics only. Sixel and the iTerm2 `OSC 1337` protocol are not
  implemented; terminals without Kitty graphics show plain text.
- The terminal (or a multiplexer in front of it) must keep graphics
  pane-local. Herdr forwards Kitty graphics through its experimental
  `kitty_graphics` option.
- Files are read whole and limited to 16 MiB; larger files stay plain text.
- No resizing, recompression or conversion: the terminal scales the original
  pixels. Animated GIF/WebP frames are whatever the terminal chooses to show.
- Each scroll step that changes the visible set re-sends that image's payload.
  Four recently used files are cached in memory.
- Candidate lines are `stat`ed on every frame until cached: a line that ends in
  an image extension but names no file costs one failed lookup per redraw.
- A click only copies a placed image. A band that is scrolled out of view or
  reduced to its caption is not clickable, and WebP is not an AppleScript image
  class, so on macOS it falls back to copying the path.
- The clipboard write shells out per click; there is no in-process clipboard
  handle.
- The file cache is keyed by path only, so editing an image in place is not
  picked up until the process restarts (`terminal_image_reset` clears it).

## Tests

`make test` covers the reserved layout order (spacers, picture band, caption
row), the signature-checked clipboard MIME type, whole-line detection (plain, quoted, bulleted, indented),
rejection of prose/unknown extensions/missing files/extension-only matches,
PNG and GIF header parsing, the 320–640 px clamp, aspect-ratio rows, narrow-pane
shrink, the exact escape sequence, the placement cache, and `linemap` row
reservation.

`make test-tui-input` covers the real rendering path twice over. The
`terminal_image` case appends an image path in a fixture reply and asserts the
exact Kitty placement (`a=T,f=100,c=...,r=...` inside a save/restore pair after
`a=d,d=A`), the PNG payload, the still-visible path line, and that an idle frame
does not re-transmit the payload. `terminal_image_scroll` covers the regression
where an off-screen band shifted every row below it (the text under the image
must still reach the bottom of the pane, and wheel-scrolling to the band must
place the image). `terminal_image_off` covers the degradation path: no graphics
escape at all and the path stays plain text. `terminal_image_click` stubs
`osascript` in `PATH`, clicks the placed band, and asserts the clipboard command
received the image path with the `PNGf` class, the `Image copied` popup, and that
no extra placement was emitted (no blink, no re-transmission). Because the drawing loop now reads wrapped
lines from the line map, the existing Markdown, shell folding, task and pager
cases exercise the restructured loop.
