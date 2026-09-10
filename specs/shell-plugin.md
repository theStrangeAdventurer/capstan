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

Shell stdout and stderr appear below the command in conversation history for
both manual `/shell` and model tool calls, including nonzero exits and timeouts.
The result includes `[exit N]`, an optional timeout notice, stdout, and a
`stderr:` section when present. Empty output leaves just the exit status.
The same result is returned to the model. Common credentials and sensitive HTTP
headers are redacted before displaying, logging, or bounding output.

The shell plugin writes one `tool` / `shell result` event at `info` level with
the command label and output, for both manual and model calls (including silent
subagents). Low-level C shell events still record execution metadata only.
`agent/tool_output.lua` owns the shared UTF-8 sanitization and output bounds:
`tool_output.max_bytes` (default 50 KiB) and `tool_output.max_lines` (default
2000). These existing settings also bound shell history and log output, with
an explicit truncation notice rather than silently dropping the result.
The model dispatcher continues to bound model results independently; silent
runs do not append output to the TUI, and CLI/ACP callback formats are unchanged.

Manual curl command labels are summarized as `curl <url>` when possible;
model-call status and execution logs retain the redacted command.

## Result display and folding

Tagged shell results use the TUI's gray text color (dim on monochrome terminals),
for manual and model calls alike. Shell output is literal: diff-like text does
not override this color. Results appear immediately, without animation.

Every nonempty tagged result with an exact `[exit 0]` status starts collapsed,
including short output and long single-line JSON. Nonzero, missing, malformed,
or otherwise unknown statuses (including timeout notices) start expanded so
errors remain visible. All nonempty bodies have the same expandable/collapsible
controls; empty bodies have none. Command and exit/timeout status stay visible;
a `[+] N lines` control replaces the collapsed body, while `[-] N lines`
precedes an expanded body.
The display projection places `[exit N]` and any timeout notice on the command's
line, with the fold control immediately on the next line (no blank separator).
This also applies to short or empty results, for both manual commands and model
tools; original UI text, model results and logs are not reformatted.
The `[+]` and `[-]` markers are bold, retaining the same gray color as the
output (dim on monochrome terminals); the line count stays gray and non-bold. Click the three-character marker to expand; `[-]`
collapses it again. Internal
blank lines count, trailing separator newlines and command/status lines do not;
terminal wrapping does not affect folding or line counts. Each result toggles separately.
Clicks work in both the main event loop and nested blocking waits, without
submitting or changing the input draft. Toggling is immediate and preserves the
viewport top where content bounds allow.

`shell_output.c` owns range metadata, line counting, display projection and
control hit testing. Only explicitly tagged shell ranges participate; ordinary
messages containing `[+]` or `[exit N]` are not guessed to be shell output.
Layout and selection/copy use the same projected text and offsets.
The original UI text, model context, logs, CLI/ACP and session text remain intact.
Fold state and ranges are local to the live TUI, not serialized: restored legacy
session text remains plain and complete. Allocation failure shows full output.

`make test` covers the 20/21-line boundary, empty output, blank lines, UTF-8,
multiple ranges, mapping and controls. The terminal fixture checks gray output,
click-to-expand/collapse, manual results, and input during a nested wait.

## Live result tagging

`agent.append_ui(text, role, "shell")` tags a live result byte range; the Lua
UI adapter forwards this optional hint only for shell completion/error output.
Manual `/shell` results are tagged when flushed from the existing context
buffer into history, not when the buffered badge appears. The shell handler's
optional fourth return value is `{shell_output_start = N}`, a zero-based byte
boundary after the complete UI command header. The C plugin adapter validates
and carries it through the context buffer. Missing or invalid metadata leaves
text untagged and complete. No first-newline heuristic is used: multiline
commands and heredocs containing `[exit ...]` cannot hide the actual status.
The model dispatcher ignores this metadata; its live range already excludes
command text. Ordinary messages,
other tools and legacy sessions are never inferred to be shell results from
text. Explicit byte ranges are persisted in sessions and restored with long
outputs collapsed by default; display caches and expansion state are transient.
Range metadata is not sent to models, logs, CLI or ACP.

The TUI renders results and status lines immediately, with no reveal timing,
highlight or animation-driven redraws. `make test-http-lua` covers live range
tagging, model-context isolation and session restoration. `make test-tui-input`
checks bold fold markers, gray line counts/body text and click handling.

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

In benchmark runs, statically visible shell paths must stay inside the
workspace. This includes redirection targets both before and after
the command token, such as `</workspace/input command` and
`command >/workspace/output`. Static `cd` targets update the effective working
directory used to validate later command segments, so paths such as `cmake ..`
are resolved from the nested directory. Missing or dynamic `cd` targets fail
closed because their resulting directory cannot be proven to stay in scope.
Operators are recognized without spaces (`printf x>out`, `cat<input`,
`2>>errors`, `&>out`). Quoted or escaped `>`/`<` characters remain word data;
quoted filenames retain spaces. Leading redirections do not consume the command
position, and chained redirections each have their target checked. Numeric file
descriptor duplication (`2>&1`) is not a path. Heredoc bodies and here-string
operands are data, not redirection filenames; command substitution in unquoted
heredocs is rejected. Unterminated quotes, heredocs, or missing redirection
operands fail closed. This remains a lexical check of visible paths, not a full
shell interpreter or protection against arbitrary code and filesystem races.

The handler reports a successful tool result only when the process exits zero
without timing out. Completion-review validation therefore ignores failed test,
lint, typecheck, build, and direct compile commands. Successful invocations are
classified from command positions and known runner/build subcommands, so a
search such as `rg test src` is not mistaken for validation merely because an
argument contains `test`. Direct compiler commands such as `javac ... && java
...` do count when the complete shell call succeeds.

The agent loop has a separate runaway guard for repeated shell commands. By
default this shell-specific guard is disabled because normal coding workflows
often repeat commands such as `pwd`, `git status`, or `make test`. When
`agent.max_same_shell_command` is set to a positive value, only consecutive
identical shell commands count toward the threshold, and the over-limit command
is stopped before permission checks and before spawning a process.

## Tests

`make test-http-lua` covers manual command joining, timeout parsing, shell output
in UI/log/model paths, success and failure with both permission paths, empty
output, timeout diagnostics, configurable byte/line bounds, secret redaction,
curl command labels, and repeated-command guard behavior.
`make test-build` checks the standalone embedded runtime, including the shared
output-bounding module.
