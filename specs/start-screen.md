# Start Screen

## Behavior

When the conversation is empty, Capstan shows a borderless, typographic start
screen. The wordmark, runtime status, and hints share one left edge within a
centered content block, rather than being centered independently.

The wide `CAPSTAN` wordmark restores the original geometric 6×8 letters with
two-pixel strokes, chamfered corners, and two blank columns between letters
(54 columns total). Pairs of pixel rows produce four terminal rows.
The bitmap matches the pre-eye wordmark, including its two-row A crossbars.

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
There is no eye, blinking, catchlight, particle, dot pattern or dithering.
Only brightness changes; letter geometry and the rest of the screen stay fixed.

All terminals use the original `▀`, `▄`, `█` glyphs with the default
background. Animation changes only foreground brightness, never glyph shape.
In particular, a lower-only edge uses `▄`, not `▀` with default foreground:
default foreground is not transparent and would fill the empty upper half.
On 256-color terminals six foreground-only color pairs are used. Palette indices are
245 through 250, in increasing brightness order. These close neutral grays
keep the reflection soft, without black patches or sharp white highlights.
Pixels outside strokes retain the terminal's default background, including
letter counters and gaps. This neutral palette is a logo-only exception;
white may lose contrast on light themes. Other terminals use `▀`, `▄`, `█`
with dim/normal/bold default foreground at cell resolution: less detail,
but no color-pair dependency or change to the letter silhouette.

Animation time starts after a 500 ms opening hold. The TUI supplies monotonic timestamps; pure animation state owns the
opening time, resetting when conversation messages replace the screen.
Ordinary redraws and layout changes do not restart it. Clock failure leaves
the wave at its initial position. No Braille glyphs are used.

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
  14 rows high and 56 columns wide
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

The wordmark bitmap, wave geometry, timing and masking policy live
in `src/start_screen.c`; ncurses
renders packed half-block cells and derives animation time from the monotonic
clock. Runtime values come from `agent_provider_name()`,
`agent_provider_model()`, `agent_reasoning_effort()`, `agent_profile_name()`,
and `app_workdir()`.

## Tests

`make test` covers layout selection, centered content bounds across terminal
sizes, wordmark bounds and letter spacing, wave travel and smoothness across
neighboring pixels and frames (including the loop boundary), cycle/reopening,
pixel masking and all six gray levels, `$HOME` path collapse, UTF-8-safe
truncation, and status/hint formatting.
`make test-tui-input` checks all six foreground shades, default backgrounds
and the original block glyphs across a full wave cycle
and the attribute fallback on 16-color terminals, compact layout, neutral
tasks, accented profile/keys, muted border/version, user gutter and purple status
dot. Light/dark contrast needs visual review.
`make` checks the ncurses renderer compiles. Visual review should check softness
of the wave, wide/compact terminals and transition to chat.
`make test-build` verifies standalone embedded runtime assets.
