# Permissions

## Behavior

Permissions protect tool calls initiated by the model. They are not an
authentication, login, password, or user-account mechanism.

Manual slash commands entered by the user, such as `/shell`, `/write`, `/file`,
or `/fetch`, are treated as direct user intent and do not go through the
permission prompt. The user already chose to run that command.

Agent-initiated tool calls go through `permit.check(tool, target)` before the
plugin handler runs. If plugin metadata declares `tool.permission`, that value
is used as the permission tool name; otherwise the model-facing tool name is
used. The decision is one of:

- `allow`: run the tool immediately.
- `deny`: skip the tool and return a denial result.
- `ask`: show the permit confirmation popup.

The permit popup offers:

- `Yes`: allow this one tool call.
- `No`: deny this one tool call.
- `Always allow`: allow this call and persist an exact rule for future calls.
  For `shell`, the persisted target is the active workspace directory, so later
  shell commands in that workspace do not prompt again just because the command
  text changed.

The selected choice can be changed with `h`/`k`, left/up arrows,
`j`/`l`, or right/down arrows. `Enter` and `Tab` confirm the current choice.
`y`, `n`, and `a` are shortcuts for `Yes`, `No`, and `Always allow`. `Esc`
denies the call.

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
    { tool = "file_read", pattern = "/repo *", allow = true },
  },
}
```

Permission rules are Lua table entries with `tool`, `pattern`, and `allow`
fields. Newer matching rules take precedence because permission checks scan from
the end of the in-memory list.

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

## Default Policy

If no configured or persisted rule matches:

- `shell` returns `ask`.
- `file_read` returns `allow` when the normalized target stays inside the
  configured [workspace directory](workspace-directory.md).
- `file_read` returns `ask` when the normalized target escapes the configured
  workspace directory.
- All other tools return `ask`.

Pattern matching supports exact target matches and a narrow wildcard form:
`"prefix *"` matches targets that start with `prefix`. The current
`Always allow` UI saves the exact target, not a wildcard.

## Architecture

`agent/tools.lua` applies permissions while processing model tool calls.
For `shell`, it uses `capstan.workdir` as the target. Other tools derive the
target from common tool arguments (`command`, `path`, `url`, `uri`) or fall back
to the tool name.

`src/permit.c` owns rule loading, saving, matching, config-rule import, and the
Lua-facing `permit` table. `src/tui.c` renders the blocking permit confirmation
popup.

Plugin metadata may declare a `permission` field to share permission policy
between related tools. For example, `file_edit` uses `file_write` permission
rules while still executing the `file_edit` handler.

## Tests

Provider-level tests cover permission target selection for `fetch`, `file_read`,
and `shell`; streamed execution for `file_edit`; permission aliases; malformed
tool arguments; and runtime log tests cover permission check/prompt logging.

Pure permission matching and saved-rule string escaping can be tested through
`permit_logic.c` without linking ncurses, Lua, or curl.
