# Terminal Runtime

Capstan should start in ordinary terminal, tmux, and screen setups without
requiring users to export `TERMINFO` or `TERMINFO_DIRS` manually.

## Behavior

- Startup requires `TERM` to be set.
- Before opening the ncurses TUI, Capstan verifies that the current terminal type
  can be resolved through terminfo or the built-in ncurses fallbacks.
- If terminal setup fails, Capstan prints a plain stderr diagnostic and exits
  before calling `initscr()`.
- The diagnostic includes the current `TERM` and suggests rebuilding with
  `./build.sh` if the vendored ncurses fallbacks are stale.

## Build Contract

`build.sh` configures vendored ncurses with:

- portable system terminfo search paths instead of a build-machine absolute
  project path;
- built-in fallback descriptions for `xterm-256color`, `tmux-256color`,
  `screen-256color`, `xterm`, `screen`, `ansi`, and `vt100`;
- a local build marker under `vendor/ncurses-install/` so old ncurses builds are
  rebuilt when the configured fallback/search-path contract changes.

The fallback entries are compiled into the static ncurses archive. They do not
make every possible terminal type available, but they cover the common terminal
types used by macOS terminals, tmux, and screen-compatible environments.

## Constraints

- Capstan still respects explicit user overrides through `TERMINFO` and
  `TERMINFO_DIRS`.
- The project does not bundle the full terminfo database beside the binary.
  Keeping the default path single-binary-friendly is preferred over shipping a
  large runtime directory.
- Unsupported `TERM` values should fail with a clear diagnostic rather than a
  raw ncurses error.

## Tests

`make test-build` verifies the standalone binary and embedded assets. Manual
terminal checks should cover:

- `TERM=xterm-256color`
- `TERM=tmux-256color`
- `TERM=screen-256color`
- no `TERMINFO`/`TERMINFO_DIRS` in the environment
- an unsupported `TERM`, which should produce the explicit diagnostic
