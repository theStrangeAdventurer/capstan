# Shell Plugin

## Behavior

`/shell <command>` executes a local shell command in `capstan.workdir`.

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

Manual slash commands bypass model-tool permission prompts because the user
directly chose the command. Model-initiated shell tool calls still go through
normal shell permissions.

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
