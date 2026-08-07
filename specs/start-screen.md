# Start Screen

## Behavior

When the conversation is empty, Capstan shows a branded start screen instead of
plain centered text. The wide layout uses a thin dim-gray border, a
black-on-white 38-column Braille Capstan illustration on the left, and a status
column on the right.

The status column shows current runtime information that is already available to
the TUI:

- active provider/model, or `not configured`
- effective reasoning effort, or `default` when the provider/model chooses it
- active profile, falling back to `implement`
- active workspace directory, collapsed under `$HOME` as `~/...`
- ready hint: `type your question or / + Tab for options`

The title uses `◉ CAPSTAN <version>`. Local builds default to `local`; release
builds receive the Git tag through `APP_VERSION`.

The start screen disappears as soon as the first user or agent message exists.

## Layout

The renderer chooses among three layouts based on the available message window:

- wide: art/status frame for at least 15 rows and 81 columns, expanding up to
  104 columns when space is available
- compact: framed text/status view for medium terminals
- minimal: legacy centered title/tagline for very small terminals

Long status values are UTF-8-aware and truncated with `...` so they do not spill
outside the frame.

## Architecture

`src/start_screen.c` owns testable formatting and layout-selection logic. The
ncurses-specific drawing stays in `src/tui.c`.

The ANSI design artifact is a visual reference only. Runtime rendering uses
static UTF-8 art strings and live values from `agent_provider_name()`,
`agent_provider_model()`, `agent_reasoning_effort()`, `agent_profile_name()`,
and `app_workdir()`.

## Tests

`make test` covers layout selection, `$HOME` path collapse, UTF-8-safe
truncation, and status fallback formatting. `make test-build` verifies the
standalone binary still starts with embedded runtime assets.
