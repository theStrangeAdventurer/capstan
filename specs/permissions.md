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

The selected choice can be changed with `h`/`k`, left/up arrows,
`j`/`l`, or right/down arrows. `Enter` and `Tab` confirm the current choice.
`y`, `n`, and `a` are shortcuts for `Yes`, `No`, and `Always allow`. `Esc`
denies the call.

Persisted rules are stored in:

```text
~/.config/capstan/permissions.lua
```

Rules are Lua table entries with `tool`, `pattern`, and `allow` fields. Newer
matching rules take precedence because permission checks scan from the end of
the in-memory list.

## Default Policy

If no persisted rule matches:

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

`ai/providers.lua` applies permissions while processing model tool calls. It
derives the target from common tool arguments (`command`, `path`, `url`, `uri`)
or falls back to the tool name.

`src/permit.c` owns rule loading, saving, matching, and the Lua-facing
`permit` table. `src/tui.c` renders the blocking permit confirmation popup.

Plugin metadata may declare a `permission` field to share permission policy
between related tools. For example, `file_edit` uses `file_write` permission
rules while still executing the `file_edit` handler.

## Tests

Provider-level tests cover permission target selection for `fetch` and
`file_read`, streamed execution for `file_edit`, permission aliases, malformed
tool arguments, and runtime log tests cover permission check/prompt logging.

Pure permission matching can be tested through `permit_pattern_match` without
linking ncurses, Lua, or curl.
