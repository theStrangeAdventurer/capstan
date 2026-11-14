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
- The TUI enables bracketed paste mode (`?2004`) so pasted newlines are inserted
  into the input buffer instead of submitting one message per pasted line.
  Startup and shutdown both disable the mode first to recover from stale terminal
  state. ncurses recognizes the start marker as a dedicated key; while pasting,
  key decoding is disabled so escape sequences in the body remain literal text.
  Both the main loop and blocking waits use the same incremental body decoder,
  restoring key decoding only after the end marker. This also supports dictation
  tools that insert text using terminal bracketed paste.
- Permission dialogs consume the same paste stream, including a paste already
  active when the dialog opens. Pasted text stays in the draft; it cannot select
  or confirm a permission. A paste started inside the dialog follows the same
  rule. Key decoding is restored on both the dialog and main window at the end.

## Mouse selection

The main message pane highlights text while the left button is held, not only
on release. ncurses delivers presses immediately (`mouseinterval(0)`) and accepts
`REPORT_MOUSE_POSITION`; explicit terminal mode 1002 requests button-held motion
because terminfo's XM commonly enables only mode 1000. Hover tracking (1003) is
not enabled. The press sets the existing visual-selection anchor; motion updates
only its cursor through the existing cell/Unicode mapping and redraw path.
Release retains the existing copy behavior. Motion without a message-pane press
is ignored; outside-pane motion leaves the last in-pane endpoint unchanged.
Shutdown and crash recovery already disable mode 1002. CLI/ACP, model context,
logs, persistence and plugin APIs are unchanged. Terminals must forward mouse
motion; native terminal selection modifiers may bypass application tracking.

`make test-mouse-selection` feeds SGR and legacy mouse reports to the real binary
in a PTY and checks reverse-video output **before release**, repeated movement,
range contraction and copy on release. Physical terminal forwarding still needs
manual verification.

## Session titles

The interactive TUI publishes the active session name using standard OSC 0
(`ESC ] 0 ; title BEL`), including named/restored sessions, session switches,
first-message names and generated titles. It writes only when the sanitized title
changes, never in CLI/ACP output or redirected stdout. C0/C1 controls and invalid
UTF-8 are removed, and the payload is bounded without splitting UTF-8 characters.
UTF-8 decoding does not depend on the process locale. On exit Capstan clears the
OSC title, then restores the prior title using the xterm title stack. Terminals
without CSI 22/23 support (including Herdr 0.9.5) get an empty title rather than
a stale session name; restoring the prior title requires stack support.
SIGINT (including Ctrl+C) and SIGTERM request main-loop shutdown, which releases
Herdr, restores the title and closes the TUI before re-raising the signal.
The signal handler only sets a shared TUI flag. Permission and choice modals
observe it before and after input waits, cancel as `deny` / `NULL`, release their
windows, and refuse to open subsequent prompts while shutdown unwinds. Other
blocking callbacks must still return before cleanup can finish. CLI/ACP signal
behavior is unchanged. `make test-terminal-title` covers both signal types while
idle and in both modal prompts, including cancellation, terminal attributes,
OSC restoration and Herdr release.

Herdr 0.9.5 can display these names independently of tab names:

```toml
[ui.sidebar.agents]
rows = [["state_icon", "terminal_title_stripped"], ["agent", "workspace"]]
```

Other terminal emulators and multiplexers that consume OSC titles work without
Herdr-specific configuration in Capstan. `make test` covers sanitization;
`make test-terminal-title` checks actual TUI OSC output in a pseudo-terminal.

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

- The interactive runtime captures terminal attributes before `initscr()`.
  On SIGABRT, SIGSEGV, SIGBUS, SIGILL, or SIGFPE, `terminal_guard` makes a
  best-effort restoration of those attributes, disables mouse/paste modes,
  leaves the alternate screen and shows the cursor. It then re-raises the
  original signal with default disposition, preserving OS crash diagnostics.
  The handler uses only async-signal-safe operations; it does not save sessions
  or invoke ncurses, Lua, logging, or heap cleanup. Forked children do not
  restore their parent's terminal. CLI/ACP modes do not install this guard.
  SIGKILL and a terminal that no longer accepts writes cannot be recovered.
  `make test-terminal-guard` exercises restoration in a pseudo-terminal.

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
