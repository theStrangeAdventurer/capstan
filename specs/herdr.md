# Built-in Herdr agent reporting

`src/herdr.c` owns a narrow native adapter; it needs no plugin configuration.
Call `herdr_tick(active_session_id, state, resume_argv)` on the main thread.
The TUI reports `working` while the root agent is running and `idle` otherwise.
Permission and choice dialogs override this with `blocked` for their lifetime,
including their input/paste wait loops. On acceptance, rejection or cancellation,
the TUI reports the current root state again (not a saved, potentially stale
state). A modal depth keeps any nested render blocked as well. Failed dialog
creation and shutdown follow-up prompts do not enter the blocked state.
`herdr_shutdown()` releases on normal exit and main-loop SIGINT/SIGTERM shutdown,
not session switches.

Only `HERDR_ENV=1` plus nonempty `HERDR_BIN_PATH`, `HERDR_PANE_ID`, and
`HERDR_SOCKET_PATH` enable reporting. The inherited socket environment is passed
through to the Herdr CLI. Reports identify both agent and source as `capstan`
and include the active session ID when nonempty. State reports attach literal
resume argv after `--`: `capstan --session-id ID --workdir DIR --workspace ROOT`,
plus the current profile/provider/model/reasoning effort, and explicit startup
max-turns, no-MCP, no-wiki, no-preserve-reasoning and yolo options. The session ID
is explicit: restoration does not depend on the workspace's active-session
pointer. Changed model/profile/paths also invalidate the successful-report cache.

Herdr requires `capstan` to be installed on the restored pane's `PATH` (not an
absolute binary path), at most 64 arguments and 8 KiB including argument NULs,
and no apostrophes or control characters. The adapter validates these limits
before spawning. Invalid resume argv omit only the resume command; status reports
continue. This means paths containing apostrophes cannot be restored by this
contract. CLI startup flags are preserved; subsequent runtime-only permission
changes and unsaved configuration are not captured. Conversation recovery is
limited to the last durable session snapshot. No sequence number is supplied.

Commands use `posix_spawn` with literal argv and all standard streams redirected
to `/dev/null`. A single report runs at a time; ticks reap with `WNOHANG` and
send only the latest state/session after the previous report exits. Hung reports
are killed after two seconds as observed by subsequent ticks. Failures are
silent; only a zero exit status commits the sent state/session. Failed spawns,
nonzero exits and killed commands retry the latest state after a one-second
monotonic cooldown, without requiring another state/session change. Shutdown waits at most roughly
100 ms each for an in-flight report and a best-effort release, killing either
command on timeout. Waiting for release prevents session-leader exit from
immediately interrupting it with SIGHUP. A server already processing the killed report
may race with release; Herdr's idle-shell cleanup remains the fallback.

Focused standalone test (no real Herdr panes or socket access):

```sh
make test-herdr
make test-terminal-title
```

The adapter fixture verifies environment gating, literal argv, all supported
states, session and resume-command changes, duplicate suppression, retry cooldown,
resume validation boundaries, and release. It deliberately writes to stdout/stderr
to exercise redirection. The standalone test includes the adapter implementation
so it can coordinate on actual child completion and the committed cache; it does
not assume the child finishes within a fixed sleep. Deadline-bounded polling
still fails if the child genuinely hangs. Production compilation picks up
`src/herdr.c` through `SRCS = $(wildcard src/*.c)`.

The isolated PTY fixture checks permission/choice acceptance and cancellation,
restoring idle or working state, blocked reporting throughout modal waits,
SIGINT/SIGTERM modal cancellation and release. It inspects resume argv, then
executes it through a fixture PATH to restore the original titled session after
a different session became active. No real Herdr socket or LLM requests are used.
A real Herdr server stop/start remains a manual integration check.

Contract: https://herdr.dev/tui/docs/add-herdr-support/
