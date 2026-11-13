# Built-in Herdr agent reporting

`src/herdr.c` owns a narrow native adapter; it needs no plugin configuration.
Call `herdr_tick(active_session_id, state)` each render on the main thread.
The TUI reports `working` while the root agent is running and `idle` otherwise.
The adapter also accepts `blocked`, but permission-dialog reporting is not wired
by this feature. `herdr_shutdown()` releases on normal exit and main-loop
SIGINT/SIGTERM shutdown, not session switches.

Only `HERDR_ENV=1` plus nonempty `HERDR_BIN_PATH`, `HERDR_PANE_ID`, and
`HERDR_SOCKET_PATH` enable reporting. The inherited socket environment is passed
through to the Herdr CLI. Reports identify both agent and source as `capstan`
and include the active session ID when nonempty. No resume command or sequence
number is supplied.

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
cc -Iinclude -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE src/herdr.c test/test_herdr.c -o build/test_herdr
./build/test_herdr
```

The fixture verifies environment gating, literal argv, all supported states,
session changes, duplicate suppression, and release. It deliberately writes to
stdout/stderr to exercise redirection. Production compilation already picks up
`src/herdr.c` through `SRCS = $(wildcard src/*.c)`; add a standalone test target
rather than adding this test's `main` to the munit runner.

## Remaining integration work

This is the first usable integration, not the complete Herdr lifecycle contract.

- Report `blocked` while waiting for permission, then restore the current state
  when the user decides. The current TUI keeps reporting `working` during a prompt.
- Supply the resume command required to reopen the same Capstan conversation
  after a Herdr restart. A session ID alone is not enough.
- Replace timing-dependent waits in `test-herdr` with deterministic coordination.
  A parallel test run has failed even though the standalone run passed; do not
  treat the current fixture as proven stable under contention.

Contract: https://herdr.dev/tui/docs/add-herdr-support/
