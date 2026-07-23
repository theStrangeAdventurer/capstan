# Shell Plugin

## Behavior

`/shell <command>` executes a local shell command in `capstan.workdir`. The
workspace permission target remains `capstan.workspace_root`, which may be an
ancestor of that working directory.

Manual slash-command input treats everything after `/shell` as the command
string. The user does not need to wrap the whole command in quotes:

```text
/shell ls -la src
```

The optional manual timeout syntax is:

```text
/shell --timeout 10 make test
/shell -t 10 make test
```

Model tool calls use structured arguments:

```json
{ "command": "make test", "timeout": 60 }
```

Timeouts terminate the whole shell process group, including pipeline children.
Capstan first sends `SIGTERM`, then escalates to `SIGKILL`, and never waits
indefinitely for inherited stdout/stderr descriptors to close.

Shell stdout and stderr are returned to the model as tool results, but common
credentials and HTTP headers are redacted first. Curl commands are summarized in
UI/log/tool-call history as `curl <url>` when possible; raw curl headers and
options are not shown in the conversation status.

## Security

The shell tool may receive commands containing credentials, especially `curl`
headers. Capstan redacts common sensitive headers and key/value pairs from:

- shell plugin UI and LLM results;
- runtime tool logs;
- assistant tool-call arguments replayed in continuation requests.

Continuation requests preserve non-sensitive shell command text so the model can
reason about what it already ran. Redaction must remove credentials without
collapsing ordinary commands to placeholders.

Manual slash commands bypass model-tool permission prompts because the user
directly chose the command. Model-initiated shell tool calls still go through
normal shell permissions.

In workspace-scoped full-control runs, statically visible shell paths must stay
inside the workspace. This includes redirection targets both before and after
the command token, such as `</workspace/input command` and
`command >/workspace/output`. Static `cd` targets update the effective working
directory used to validate later command segments, so paths such as `cmake ..`
are resolved from the nested directory. Missing or dynamic `cd` targets fail
closed because their resulting directory cannot be proven to stay in scope.

The handler reports a successful tool result only when the process exits zero
without timing out. Completion-review validation therefore ignores failed test,
lint, typecheck, and build commands.

The agent loop has a separate runaway guard for repeated shell commands. By
default this shell-specific guard is disabled because normal coding workflows
often repeat commands such as `pwd`, `git status`, or `make test`. When
`agent.max_same_shell_command` is set to a positive value, only consecutive
identical shell commands count toward the threshold, and the over-limit command
is stopped before permission checks and before spawning a process.

## Tests

`make test-http-lua` covers manual command joining, timeout parsing, shell output
redaction, curl header redaction, runtime log redaction, hidden command status
text, and repeated-command guard behavior.
