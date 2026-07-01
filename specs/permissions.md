# Permissions

## Behavior

Permissions protect tool calls initiated by the model. They are not an
authentication, login, password, or user-account mechanism.

Manual slash commands entered by the user, such as `/shell`, `/write`, `/file`,
or `/fetch`, are treated as direct user intent and do not go through the
permission prompt. The user already chose to run that command.

Agent-initiated tool calls first pass the active profile's tool availability
gate. If a model returns a tool call that was not in the filtered tool list for
the active run, Capstan returns an unavailable-tool result without checking
permissions or running plugin handlers.

Available agent-initiated tool calls go through `permit.check(tool, target)`
before the plugin handler runs. If plugin metadata declares `tool.permission`,
that value is used as the permission tool name; otherwise the model-facing tool
name is used. The decision is one of:

- `allow`: run the tool immediately.
- `deny`: skip the tool and return a denial result.
- `ask`: show the permit confirmation popup.

The permit popup offers:

- `Yes`: allow this one tool call.
- `No`: deny this one tool call.
- `Tool run`: temporarily allow this permission tool for the current run scope.
- `Full run`: temporarily allow all permissioned tools for the current run scope.
- `Always allow`: allow this call and persist an exact rule for future calls.
  For `shell`, the persisted target is the active workspace directory, so later
  shell commands in that workspace do not prompt again just because the command
  text changed.

The selected choice can be changed with `k`, up arrow, `j`, or down arrow.
`Enter` and `Tab` confirm the current choice. Mouse clicks on a choice select
that choice; mouse clicks outside the permit popup are ignored.
`y`, `n`, `t`, `f`, and `a` are shortcuts for `Yes`, `No`, `Tool run`,
`Full run`, and `Always allow`. `Esc` denies the call.

Declarative permission rules are configured in:

```text
~/.config/capstan/config.lua
```

under the `permissions` key:

```lua
return {
  capabilities = {
    self_improvement = true,
  },
  permissions = {
    { tool = "file_read", pattern = "~/narnia/tui-agent/*", allow = true },
    { tool = "fetch", pattern = "https://api.openai.com/*", allow = true },
  },
}
```

Permission rules are Lua table entries with `tool`, `pattern`, and `allow`
fields. Newer matching rules take precedence because permission checks scan from
the end of the in-memory list. A leading `~` or `~/` in `pattern` expands to the
current `HOME` directory.

Runtime choices from `Always allow` are not editable config. They are persisted
as application state in:

```text
$XDG_STATE_HOME/capstan/permissions.lua
```

When `XDG_STATE_HOME` is unset, the fallback is:

```text
~/.local/state/capstan/permissions.lua
```

Config rules load before runtime-state rules, so choices saved by the runtime
prompt still take precedence. Saved `tool` and `pattern` values are escaped as
Lua string contents so quotes, backslashes, control characters, and newlines
cannot corrupt the state file.

`Tool run` and `Full run` are in-memory only. They are scoped to the active
agent run. A `subagents` tool call creates one shared child permission scope for
all subagent tasks in that batch, so approving `Tool run` or `Full run` once
applies across the sibling subagents without writing persistent rules.

Headless `capstan run --full-control` creates a non-persisted run scope without
showing prompts. It is workdir-only: `shell` is allowed in the active workspace,
and `file_read` / `file_write` are allowed only for normalized paths inside the
active workspace. `--benchmark` enables this mode and also skips MCP startup.

## Default Policy

If no configured or persisted rule matches:

- `shell` returns `ask`.
- `file_read` returns `allow` when the normalized target stays inside the
  configured [workspace directory](workspace-directory.md).
- `file_read` returns `ask` when the normalized target escapes the configured
  workspace directory.
- Sensitive local filenames such as `.env`, `.env.*`, or names containing
  `secret`, `token`, or `credential` force an `ask` decision for model-initiated
  file tools even when the path is inside the workspace.
- All other tools return `ask`.

The `subagents` model tool does not use a permission pattern. It is exposed by
default and hidden only when `capabilities.subagents = false`. Its scale is
controlled by `subagents.max_concurrent`, `subagents.max_tasks`,
`subagents.max_turns`, and `subagents.max_turns_cap`. Tools executed by
subagents still use their own permission checks.

Pattern matching supports exact target matches, glob-style `*` for any sequence,
and `?` for one character anywhere in the pattern:

```lua
{ tool = "fetch", pattern = "*", allow = true }
{ tool = "fetch", pattern = "https://api.openai.com/*", allow = true }
{ tool = "fetch", pattern = "https://*.example.com/*", allow = true }
{ tool = "fetch", pattern = "*://api.example.com/*", allow = true }
{ tool = "file_read", pattern = "~/notes/file?.md", allow = true }
```

A pattern ending in `/*` also matches the directory or URL root before the slash,
so `"~/narnia/tui-agent/*"` matches both the workspace directory itself and
paths inside it. This keeps one rule useful for `shell` workspace targets and
file paths. Newer rules take precedence, so place specific denies after broader
allows when both could match. The older `"prefix *"` wildcard form is still
accepted because it is now just normal `*` matching. The current `Always allow`
UI saves the exact target, not a wildcard.

## Architecture

`agent/tools.lua` first rejects unavailable model tool calls, then applies
permissions while processing available tool calls that require permission. For
`shell`, it uses `capstan.workdir` as the target. Other permissioned tools
derive the target from common tool arguments (`command`, `path`, `url`, `uri`)
or fall back to the tool name. Local file permission targets (`file_read` and
`file_write`) are normalized before matching: relative paths resolve against
`capstan.workdir`, `~/` expands through `HOME`, and `.` and `..` segments are
collapsed.

The built-in file plugins also verify real filesystem paths for model-initiated
calls when `capstan.realpath` is available. Existing targets are checked after
resolving symlinks. New write targets check the nearest existing parent
directory. A path that appears inside the workspace but resolves outside it is
rejected before reading or writing, unless it is a model-initiated read of a
registered skill file under one of Capstan's skill roots. Model-initiated reads
of explicit absolute paths outside the workspace are allowed only after the
normal permission decision allows that external target.

`src/permit.c` owns rule loading, saving, matching, config-rule import, and the
Lua-facing `permit` table. `src/tui.c` renders the blocking permit confirmation
popup.

The permit popup itself is a synchronous modal prompt. After the user confirms a
tool call, any C-side blocking work must periodically pump the TUI instead of
waiting in a raw blocking syscall. In particular, `tools.shell` keeps rendering
and handles safe scroll/resize-style input while waiting for the subprocess to
finish, then logs the shell duration and timeout status.

Plugin metadata may declare a `permission` field to share permission policy
between related tools. For example, `file_edit` uses `file_write` permission
rules while still executing the `file_edit` handler.

## Tests

Provider-level tests cover permission target selection for `fetch`, `file_read`,
and `shell`; sensitive file prompting; streamed execution for `file_edit`;
permission aliases; malformed tool arguments; and runtime log tests cover
permission check/prompt logging.

Pure permission matching and saved-rule string escaping can be tested through
`permit_logic.c` without linking ncurses, Lua, or curl.
