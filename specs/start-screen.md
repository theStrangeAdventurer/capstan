# Start Screen

## Behavior

When the conversation is empty, Capstan shows a borderless, typographic start
screen. The wordmark, runtime status, and hints share one left edge within a
centered content block, rather than being centered independently.

The wide `CAPSTAN` wordmark restores the original geometric 6×8 letters with
two-pixel strokes, chamfered corners, and two blank columns between letters
(54 columns total). Pairs of pixel rows produce four terminal rows.
The bitmap matches the pre-eye wordmark, including its two-row A crossbars.

On the first start-screen display in an application, all seven letters begin assembling together. Each has a different ordered fill: top-down, bottom-up, right-to-left,
center-out by rows, center-out by columns, outside-in, and reverse row order.
Launch times are normalized per glyph so both the first launch (0 ms) and final
arrival (2320 ms) coincide, including the two differently animated A letters. Each square follows exactly one axis,
using letter- and pixel-specific left/right/top/bottom approaches, with a 120 ms constant-speed flight.
Flights cover up to eight horizontal or six vertical pixels, starting inside
the padded canvas so all letters visibly begin together. Settled pixels are dim.
The assembly phase ends at 2600 ms, followed by a 900 ms faulty-contact phase:
only pixel (row 0, column 1, upper-left of C) makes a weak attempt, fades, retries brighter, then dips briefly before staying
lit. Unequal pauses and brightness ramps replace identical square pulses.
The whole word then lights white for 200 ms and settles over 300 ms into the
original wave. Only the wave repeats after the 4000 ms intro.

Only assembly runs at 1.5× speed (about 1.73 seconds in real time).
The contact-light phase retains 900 ms for a readable irregular startup;
ignition/settling retain 500 ms. Each subsequent wave has its original leisurely
2500 ms period. TUI and exporter use this same piecewise clock mapping.

The timeline starts on the first display, including compact layouts, and never
restarts on resize or after leaving/reopening the start screen. A clock failure
shows the complete word. All frames use the existing event loop: no waits,
threads, allocations or new runtime dependencies. Input interrupts normally.

A silk-like wave of light travels left to right inside the unchanged letter
strokes, repeating in the same direction without a return pass.
A broad curved ribbon (22-pixel half-width) uses a smoothstep
brightness envelope rather than a narrow glint. Its center bends with row
position and loops over a 108-pixel span in 2500 ms,
including an off-screen gap so it exits before reappearing smoothly on the left.
Travel follows cubic Bezier control points (0,0), (1/3,0), (2/3,1), (1,1),
equivalent to `3t² - 2t³`: it accelerates across the letters, reaches the
right edge around 1250 ms, and slows off-screen. Peak speed is 64.8 pixels/second;
velocity is zero at the loop boundary. Brightness and ribbon geometry are unchanged.
Both occupied halves of each terminal cell share one shade to avoid
speckled half-pixel highlights; half-blocks still preserve the letter edges.
After the intro there are no particles or isolated blinking pixels.
After the intro, only brightness changes; letter geometry stays fixed.

The TUI consumes a 58×12 shaded pixel canvas (two pixels of padding on each
side); the word itself remains the original 54×8 bitmap. On 256-color terminals,
paired half-pixels use separate foreground/background shades to light exactly
one square during the contact flicker. Shades 1–6 retain baseline grays 245–250;
white 255 is reserved for the intro flash. Empty halves retain default background,
and lower-only pixels use `▄`. Limited-color terminals use dim/normal/bold at
cell resolution, preserving geometry but reducing flicker detail.

Two blank rows separate the wide wordmark from the muted `version: <version>`,
with one blank row before the settings. Labels and version use gray 245 on
256-color terminals, dim default foreground otherwise. Values retain normal
foreground; the active profile is bold purple. Shortcut keys
(`/` and `Shift+Tab`, not explanations) also use purple. One blank row separates
settings from keyboard hints. There is no static `ready` indicator: it did not
represent runtime readiness. The empty input contains the
muted `Type a message to begin` hint, hidden on typing or once history exists.
Input geometry is unchanged; only its border is quieter (gray 240 with an
attribute-only dim fallback).

The status column shows current runtime information that is already available to
the TUI:

- active provider/model, or `not configured`
- effective reasoning effort, or `default` when the provider/model chooses it
- active profile, falling back to `implement`
- active workspace directory, collapsed under `$HOME` as `~/...`
- keyboard hints: `/ commands · Shift+Tab profiles`

The compact layout uses plain `CAPSTAN` with the dim version beside it and the
same status/footer rendering as the wide layout. Local builds default to
`local`; release builds receive the Git tag through `APP_VERSION`.

The start screen disappears as soon as the first user or agent message exists.

## Layout

The renderer chooses among three layouts based on the available message window:

- wide: animated wordmark for at least 20 rows and 64 columns; content is
  16 rows high and 56 columns wide
- compact: borderless text/status view for at least 12 rows and 48 columns;
  content is 9 rows high and up to 56 columns wide
- minimal: unchanged centered title for very small terminals

Wide and compact content is centered as one block with at least two columns
of horizontal padding. Long status values and versions are UTF-8-aware and
truncated with `...` within the content width. Both keyboard hints fit even in
the smallest compact layout.

## Architecture

`src/start_screen.c` owns testable formatting, layout selection, and content
geometry. The ncurses-specific drawing stays in `src/tui.c`, using a single
renderer for wide/compact status and hints. This change is presentation-only:
model context, logs, persisted state, CLI mode, and plugin APIs are unchanged.

The bitmap, axis-only trajectories, intro timeline and original wave live in
`src/start_screen.c`. TUI and `tools/preview_start_spin.c` consume the same frame;
`tools/preview_start_spin.py` uses Pillow solely to create preview artifacts.
Model context, runtime logs, persisted state and plugin APIs are unaffected.

Tests cover simultaneous assembly of all seven letters, launch order, exact axes, monotonic travel without overshoot,
assembly completion, the unique blinking pixel and three flashes, global
ignition, unchanged original wave over three periods, and no intro replay.
PTY tests check palette, compact resize, input during assembly and chat transition.
The GIF exports a single opening plus two wave cycles; replaying the GIF repeats
the recording, not the runtime intro.
