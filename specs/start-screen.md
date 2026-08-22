# Start Screen

## Behavior

When the conversation is empty, Capstan shows a branded start screen instead of
plain centered text. The wide layout uses a thin dim-gray border and a centered
`CAPSTAN` wordmark built from full block characters. Thick 5×7 letters use one
terminal cell per logical pixel so the logo remains compact. The wordmark uses a
brighter purple palette with metallic grain: base pixels in color 141 (bright
medium purple), highlight grain in color 176 (light orchid), and gradient
accents in colors 177–195 for a moving reflection sweep. A single broad,
diagonal fifteen-cell highlight sweeps across the wordmark like a reflection on
polished metal. Each sweep starts slowly, accelerates across the word, pauses
briefly while off-screen, then repeats. Its five visible highlight steps
transition through brighter purples and pale violets while preserving sharp
pixel edges. No Braille glyphs are used. The row below the wordmark is
intentionally blank, the version label sits in the top-right interior, and the
frame keeps two blank rows below the ready hint.

The status column shows current runtime information that is already available to
the TUI:

- active provider/model, or `not configured`
- effective reasoning effort, or `default` when the provider/model chooses it
- active profile, falling back to `implement`
- active workspace directory, collapsed under `$HOME` as `~/...`
- ready hint: `Type message · / + Tab for commands · Shift+Tab: fast / implement / plan`

The compact layout uses `◉ CAPSTAN`; the wide layout places the version in the
frame's top-right interior. Local builds default to `local`; release builds
receive the Git tag through `APP_VERSION`.

The start screen disappears as soon as the first user or agent message exists.

## Layout

The renderer chooses among three layouts based on the available message window:

- wide: animated wordmark/status frame for at least 20 rows and 64 columns,
  expanding up to 88 columns when space is available
- compact: framed text/status view for medium terminals
- minimal: legacy centered title/tagline for very small terminals

Long status values are UTF-8-aware and truncated with `...` so they do not spill
outside the frame.

## Architecture

`src/start_screen.c` owns testable formatting and layout-selection logic. The
ncurses-specific drawing stays in `src/tui.c`.

The wordmark bitmap and gradient policy live in `src/start_screen.c`; ncurses
renders each active pixel as `█` and derives animation time from the monotonic
clock. Runtime values come from `agent_provider_name()`,
`agent_provider_model()`, `agent_reasoning_effort()`, `agent_profile_name()`,
and `app_workdir()`.

## Tests

`make test` covers layout selection, wordmark bounds, gradient movement,
`$HOME` path collapse, UTF-8-safe truncation, and status fallback formatting. `make test-build` verifies the
standalone binary still starts with embedded runtime assets.
