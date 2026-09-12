# Process control

## Scope and user interface

The first version manages shell/argv children and local stdio MCP servers of
this Capstan instance. It is not a system-wide process monitor, daemon, LSP
client, terminal emulator or background agent scheduler.

`/processes` (no arguments), `Ctrl+P`, and the clickable, dim top-right badge
`[ Background processes: N | /processes for details ]`
open the same live C panel, without an LLM request. The count is running managed
roots, not every browser helper. The indicator and its clickable area are hidden
when no roots are running; `/processes` and `Ctrl+P` remain available, including
for completed entries. The badge keeps a one-column right margin; on narrow
terminals it drops the hint, then hides if the label cannot fit. While the
session overlay occupies the top-right corner, the badge and hit area are hidden.
The count still includes synchronous shell waits and MCP roots, not only shell
commands launched with `--background`. The panel works during a model request and
blocking shell/MCP waits. Other commands retain their busy/queue restrictions.
Hotkey/mouse opening preserves the input draft; slash invocation consumes only
the command. Permission dialogs have priority and cannot be bypassed.

- Up/Down or j/k: select; PageUp/PageDown: scroll.
- Enter: details/back; Tab in details: stdout, stderr, descendants.
- `s`: request stop; explicit `y` confirms, `n`/Escape cancels.
- Escape/q: back/close; Ctrl+P closes directly.
- MCP stop confirmation warns that tools and active calls may disconnect.

Details show opaque ID, PID, source kind, session owner, working directory,
command label and exit status. The list shows elapsed time and retains completed
entries. MCP and argv launches include the executable and launch arguments (for
example `npx -y @playwright/mcp@latest --headless`), not just `npx`. The manager
owns this formatting so the panel and model snapshots use the same label.
Whitespace/empty arguments are quoted, controls escaped or removed, and long
labels are bounded to 511 bytes with `...`. Sensitive `--key value` and
`--key=value` arguments use the shared redactor's key classification; their
entire values are masked before display truncation. No environment is included.
Existing processes need a restart to acquire the new label.
Labels/output are sanitized; they are not executable commands to copy.
Descendants show PID/PPID/name and group membership. Only the managed root has a
stop action; observed PIDs are never accepted by the stop API.

## Starting background commands

```
/shell --background npm run dev
/shell --background --timeout 120 make test
/shell --timeout 120 --background make test
```

The model uses `shell({command="npm run dev", background=true})`. A background
start returns `started`, ID, PID and current state immediately, **not successful
completion or validation**. Its default lifetime timeout is zero (unlimited).
An explicit positive timeout is capped at 300 seconds. Normal shell calls stay
synchronous with the existing default of 60 seconds and existing output format.
Both modes appear in the panel. Processes are non-interactive; stdin is
`/dev/null`. Use foreground server commands, not `nohup`, daemonization or `&`.

## Canonical ownership and adapters

`process_manager.c` owns managed PID/PGID lifecycle, wait status, deadlines,
nonblocking pipe collection and all signals after adoption. `shell_process.c`
is a synchronous compatibility wrapper. `mcp.c` retains its protocol pipes;
manager captures only stderr, never consumes MCP stdout as log text.

The manager runs without Lua/ncurses. Main TUI, headless CLI, ACP and nested
blocking pumps call its poll function. A permission modal still polls process
I/O but never runs callbacks or process panel actions. The panel itself performs
no Lua calls and never enters agent dispatch. Existing renderer descriptors
retain their existing behavior.

Each root has a runtime-random opaque ID plus monotonically increasing serial;
IDs are neither PIDs nor valid in a later Capstan instance. Captured session
ownership is installed around tool execution with restoration on errors.
Subagents inherit their parent's process owner. Global configured MCP servers
belong to `runtime`; ACP-added servers belong to their ACP session. Workdir is
captured at launch. Ownership is runtime data, never a model-supplied field.

The extensibility boundary is kind/owner/snapshot/action adapters. Future LSP
must own its protocol and readiness separately from OS process state; future
subagents must own cancellation separately from signals. Do not require future
non-process resources to have a PID. No LSP auto-start or installation is added.

## Lifecycle and failure behavior

Roots transition running -> stopping -> exited (or unknown exit code -1).
Timeout is retained separately. Normal exit preserves the actual leader status.
Stop targets the owned group: TERM, then KILL after 500 ms; pipe draining stops
after another 1500 ms. `waitid(WNOWAIT)` keeps the leader unreaped while group
signals remain possible, preventing PID reuse. ECHILD fails closed: no signal
and no false exit-zero result. A requested stop is not proof of termination.

The leader can exit while descendants retain output pipes; the group remains
managed until output closes or a stop/timeout. Before releasing the leader PID,
remaining same-group descendants are killed. Processes that daemonize into new
groups or attach to an existing personal browser are **not** guaranteed cleanup.

`process_observe.c` uses native Linux `/proc` or macOS process information,
without command-line arguments or environment contents. It verifies current
ancestry and identity; observations are best-effort and non-atomic. Inaccessible,
reparented, raced or excess entries may be omitted. No permission to signal is
inferred from a displayed name. The details view refreshes at most once/second
and shows at most 128 descendants; outside-group descendants are observed only.
An empty observation means either no verified descendants or unavailable data.

- Finishing an agent answer does not stop a background service.
- TUI `/new` or session switching keeps processes visible under their original
  owner; a model cannot read/stop another session's processes.
- Global runtime MCPs are visible to model tools; stopping uses the distinct
  `process_stop` permission, not shell or MCP permission.
- ACP session close/disconnect performs bounded cleanup for that owner (up to
  2500 ms, without Lua reentry). Close acknowledges successful cleanup only after
  owned roots are reaped; if any remain, it reports an actionable error while
  keeping those processes registered for continued cleanup.
- Normal CLI completion/application shutdown terminates managed groups. This is
  not survival after closing Capstan. Hard kill/crash cleanup is not guaranteed.
- Uninterruptible children may outlive the bounded shutdown wait; do not report
  them as successfully reaped.
- Registry, output and notifications are not persisted or restored.

## Agent API and completion delivery

`processes({action="list"})`, `processes({action="get", id=...})`, and
`processes({action="output", id=...})` are read-only and permission-free within
the captured session/runtime scope. `get` includes observed descendants.
`process_stop({id=...})` uses a separate permission target equal to the opaque ID.
It accepts no arbitrary PID and rechecks ownership in the native binding. Plan
allows inspection but not stopping or starting shell. These are model tools;
the manual interface is the C panel.

Background shell completions are marked pending in the manager, then delivered
once to the owner at the next ordinary orchestrator request boundary as runtime
state. Children do not consume the parent's notifications. No poll callback
starts a model run. Idle completions wait for the next request; evicted records
lose pending notifications. Notifications contain status metadata, not raw output,
and are logged when delivered. Failed/stopped MCPs stop advertising tools and
produce actionable errors for stale calls; they do not respawn automatically.
Reconnect explicitly through the existing MCP controls.

## Bounds and data safety

Hard native limits: 128 retained roots, 16 MiB maximum per stream and 32 MiB
aggregate reservation. Shell uses the existing 1 MiB stdout/256 KiB stderr
limits; MCP stderr is 64 KiB. Completed records are evicted first; exhaustion by
live roots rejects a new start and cleans its child. Poll reads at most 64 KiB
per stream per root per pass. There is no unbounded output buffer.

Output currently retains a **prefix**, not a tail. Overflow is explicitly marked.
During execution, only complete sanitized lines are exposed; a partial final
line appears at EOF. Terminal escape/control sequences are removed before the
shared credential redactor, including keys split across reads/ANSI sequences.
The entire retained prefix is redacted before display bounds; metadata is
redacted before bounded copies. Raw output exists only for the established
synchronous shell adapter, whose plugin applies its existing redaction policy.
The existing Lua tool-output limits also bound model results. Redaction is
pattern-based, not a promise to recognize arbitrary secrets printed by programs.

## Tests

- `make test`: lifecycle, timeout, descendant-held pipes, stale IDs, limits,
  unknown waits, live output/redaction, native ancestry and unrelated processes.
- `make test-process-control`: real Lua/native MCP and tools bindings, ownership,
  completion delivery, owner cleanup, panel controller, plugin contracts and PTY
  background/synchronous wait, live output, confirmation, mouse and draft tests.
- `make test-http-lua`: existing runtime, permission, subagent, MCP and shell
  contracts; MCP mocks now implement alive for periodic reconciliation.
- `make test-tui-input`: existing queued input, paste and modal regressions.
- `make test-build`: isolated embedded binary, ACP, release and installer smoke.

Platform validation must report actual host coverage; native macOS observation
passing does not establish Linux correctness.
