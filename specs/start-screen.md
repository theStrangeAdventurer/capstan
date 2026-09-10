# Start Screen

## Behavior

When the conversation is empty, Capstan shows a borderless, typographic start
screen. The wordmark, runtime status, and hints share one left edge within a
centered content block, rather than being centered independently.

The wide `CAPSTAN` wordmark uses bold geometric 6×8 letters with two-pixel
strokes, chamfered corners, and two blank columns between letters (54 columns
total). Pairs of pixel rows are packed into `▀`, `▄`, and `█`, producing four
terminal rows: pixels are approximately square rather than tall terminal cells.
On 256-color terminals the wordmark uses six neutral gray-to-white shades
(indices 245, 248, 250, 252, 254, 231), from gray at rest to a white highlight
core, with the terminal's default background. This deliberate logo-only exception
keeps the reflection visible without a purple tint; white can lose contrast on
light backgrounds. Other terminals retain default-foreground dim/normal/bold
reflection; its contrast depends on theme and attribute support.
The diagonal fifteen-pixel highlight accelerates across the word in 900 ms,
then pauses fully off-screen for 2700 ms. The first visible highlight begins
500 ms after the start screen is first rendered, without an off-screen run-up.
The TUI supplies monotonic timestamps; the pure animation state owns the opening
time and delay, resetting when conversation messages replace the screen.
Ordinary redraws and layout changes do not restart it. Clock failure leaves the
logo at rest. Unit tests cover the delay boundary, visible first highlight,
repeat cycle, and reopening. No Braille glyphs are used.

Two blank rows separate the wide wordmark from the dim version, with one blank
row before the settings. Values use normal weight, with the active profile
remaining bold in the default foreground. Only the ready dot is purple; its
label uses normal foreground. One blank row separates settings from the
ready line and keyboard hints. The input box below the message window is
unchanged.

The status column shows current runtime information that is already available to
the TUI:

- active provider/model, or `not configured`
- effective reasoning effort, or `default` when the provider/model chooses it
- active profile, falling back to `implement`
- active workspace directory, collapsed under `$HOME` as `~/...`
- ready line: `● ready  Type a message to begin`
- keyboard hints: `/models choose model · Shift+Tab profiles`

The compact layout uses plain `CAPSTAN` with the dim version beside it and the
same status/footer rendering as the wide layout. Local builds default to
`local`; release builds receive the Git tag through `APP_VERSION`.

The start screen disappears as soon as the first user or agent message exists.

## Layout

The renderer chooses among three layouts based on the available message window:

- wide: animated wordmark for at least 20 rows and 64 columns; content is
  15 rows high and 56 columns wide
- compact: borderless text/status view for at least 12 rows and 48 columns;
  content is 10 rows high and up to 56 columns wide
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

The wordmark bitmap and gradient policy live in `src/start_screen.c`; ncurses
renders packed half-block cells and derives animation time from the monotonic
clock. Runtime values come from `agent_provider_name()`,
`agent_provider_model()`, `agent_reasoning_effort()`, `agent_profile_name()`,
and `app_workdir()`.

## Tests

`make test` covers layout selection, centered content bounds across terminal
sizes, wordmark bounds and letter spacing, gradient movement and the off-screen
pause, `$HOME` path collapse, UTF-8-safe truncation, and status/hint formatting.
`make test-tui-input` checks all six logo shades across a full reflection cycle
and the attribute fallback on 16-color terminals, compact layout, neutral
profiles/tasks and the purple status dot. Light/dark contrast needs visual review.
`make` checks the ncurses renderer compiles. Visual review should check wide and
compact terminals, the retained reflection sweep, and the transition to chat.
`make test-build` verifies standalone embedded runtime assets.
