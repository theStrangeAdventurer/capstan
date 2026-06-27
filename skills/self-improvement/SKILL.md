---
name: self-improvement
description: Use only when a task requires extending Capstan itself with user plugins, hooks, slash commands, autocomplete, model tools, or local runtime automation.
---

# Self Improvement

Use this skill only when the user's task benefits from teaching Capstan a new
local behavior that should remain available after the current request.

Good fits:
- a reusable slash command the user can run manually;
- a model tool the agent should call in future runs;
- autocomplete for a slash command;
- an agent hook that adjusts request, tool, stream, or final-turn behavior;
- a paired plugin plus skill so the agent knows when and why to use the plugin.

Do not create durable extensions for one-off work. Prefer ordinary file edits,
shell commands, or a short explanation when the behavior does not need to
persist.

## Enablement

This skill is available only when the user has explicitly enabled it in:

```text
~/.config/capstan/config.lua
```

```lua
return {
  capabilities = {
    self_improvement = true,
  },
}
```

## Where To Write

Write user plugins as separate Lua files:

```text
~/.config/capstan/plugins/*.lua
```

Do not store plugin source code inside `config.lua`. Capstan hot-reloads new,
changed, and deleted plugin files from this directory. If a changed plugin fails
to load, the previous working version stays active and Capstan retries later.

If the agent should know when to use a new command or model tool, also create a
small skill under one of these user-controlled skill roots:

```text
~/.agents/skills/<name>/SKILL.md
~/.config/capstan/skills/<name>/SKILL.md
```

Use a paired skill when the plugin is not self-evident from its command name or
when the user wants the agent to proactively use it.

## Decide The Extension Type

- Use a slash command when the user should trigger behavior manually, such as
  `/jira ABC-123`, `/note`, or `/env`.
- Add `plugin.history = false` for control commands that only change local
  runtime state or show diagnostics. No-history commands do not trigger an
  agent request.
- Add `plugin.tool` when the model should be able to call the behavior during
  normal reasoning. Tool calls go through permissions.
- Add `plugin.autocomplete` when command arguments have discoverable values,
  such as files, models, project names, ticket ids, or saved profiles.
- Add hooks for policy that should run automatically at a pipeline stage.
- Suggest a new command when you notice a repeated workflow, a multi-step local
  operation, or a user phrase that maps cleanly to a named action. Explain why
  it is worth making durable before or after creating it.

## Plugin Contract

A plugin file returns a table. `id` is required. Use stable ids; changing the id
creates a different plugin.

```lua
local plugin = {
  id = "my_plugin",
  name = "My Plugin",
  description = "Short user-facing description.",
}

return plugin
```

For a slash command, add `command` and `handler`:

```lua
plugin.command = "/mine"

function plugin.handler(ctx)
  local value = ctx.args[1]
  if not value then
    return ctx:replace("Usage: /mine <value>")
  end
  return ctx:replace("Shown in chat", "Sent to the model")
end
```

Handler context:
- `ctx.input`: full original user input;
- `ctx.command`: matched command token, such as `"/mine"`;
- `ctx.args`: array of space-split arguments after the command;
- `ctx.tool_args`: table of decoded model-tool arguments, only for tool calls;
- `ctx:replace(ui, llm?)`: returns UI text plus optional model-context text.
  If `llm` is omitted, the UI text is also sent to the model.

`ctx.args` is a simple space-split array. For values that may contain spaces,
newlines, JSON, or exact file fragments, prefer structured model-tool arguments
or an explicit command syntax that handles those cases.

Manual slash commands do not pass through model-tool permission prompts because
the user directly chose the command. Model tool calls do.

## Model Tools

Expose a model tool by adding `plugin.tool`. The same `handler(ctx)` is used for
manual slash commands and model tool calls; read structured values from
`ctx.tool_args` first, then fall back to `ctx.args` for manual use.

```lua
plugin.tool = {
  name = "mine",
  description = "Do the durable local action.",
  parameters = {
    type = "object",
    properties = {
      value = { type = "string", description = "Value to process" },
    },
    required = { "value" },
  },
  permission = "mine",
}

function plugin.handler(ctx)
  local value = (ctx.tool_args and ctx.tool_args.value) or ctx.args[1]
  if not value then
    return ctx:replace("Usage: /mine <value>")
  end
  return ctx:replace("Processed " .. value)
end
```

`plugin.tool.permission` defaults to the tool name when omitted. Set it to an
existing permission name when the tool is really performing that kind of action;
for example, a file-writing tool should usually use `permission = "file_write"`.

Permission targets are derived from common argument names: `command`, `path`,
`url`, or `uri`. File permissions normalize relative paths against
`capstan.workdir`. Shell permissions use `capstan.workdir` as the target.

## Autocomplete

Autocomplete is attached to a command with `plugin.autocomplete`. The `fetch`
function receives the current partial args and returns either strings or
`{text=..., value=...}` items.

```lua
plugin.autocomplete = {
  title = "Mine",
  limit = 10,
  multi = false,
  fetch = function(args)
    return {
      { text = "Readable label", value = "inserted-value" },
      "plain-value",
    }
  end,
}
```

Use autocomplete only when it saves the user from remembering exact values. Keep
fetch functions fast and local unless the user explicitly asked for network
lookup.

## Hooks

Hook-only plugins may omit `command` and `handler`:

```lua
plugin.hooks_scope = "orchestrator"
plugin.hooks = {
  before_request = function(ctx)
    ctx.request.temperature = 0.2
    return ctx
  end,
}
```

Hook stages:
- `before_messages`: may change `ctx.messages`;
- `before_tools`: may change `ctx.tools`;
- `before_request`: may change `ctx.request`, `ctx.headers`, `ctx.endpoint`;
- `on_stream_chunk`: may change or clear `ctx.chunk`;
- `before_tool_call`: may change `ctx.name`, `ctx.args`, `ctx.target`,
  `ctx.permission_tool`;
- `after_tool_call`: may change `ctx.result`;
- `after_agent_turn`: receives final text after all tool continuations finish;
- `after_subagents`: may change the structured subagents result.

Do not replace `_G.agent_entry` for normal customization. Use hooks.

Hook scope:
- `plugin.hooks_scope = "orchestrator"` runs plugin hooks only for the top-level
  agent. Use this for notifications.
- `plugin.hooks_scope = "subagents"` runs plugin hooks only inside subagents.
- `plugin.hooks_scope = "all"` runs plugin hooks everywhere.
- Per-hook table entries may set `scope = "orchestrator"`, `"subagents"`, or
  `"all"` to override the plugin default.

`after_agent_turn` defaults to orchestrator-only because it usually means
"notify the user after the whole turn is done." Set `scope = "all"` only when
subagent completions should also trigger the hook.

## Implementation Rules

- Keep extensions narrow, reversible, and named for the user workflow.
- Reuse built-in helpers when available: `http.get`, `http.post`,
  `http.post_stream`, `tools.shell`, `agent.append`, `capstan.state_path`,
  `capstan.config_path`, `capstan.workdir`, and `capstan.log`.
- Validate inputs and return clear `Usage:` messages.
- Prefer structured tool parameters over parsing free-form strings.
- Keep blocking work short. Long waits should use existing helpers that keep the
  UI responsive.
- Do not create background network calls, shell execution, or persistent writes
  unless the user task clearly requires them.
- If a plugin writes files, uses shell commands, reads credentials, or changes
  agent behavior durably, state that in the final response.
- If a plugin fails to load, preserve the previous working version and fix the
  file rather than deleting user work.

## Verification

After writing or editing a plugin:
- run the most focused tests available, usually `make test-http-lua` for Lua
  plugin behavior and `make test-build` when embedded assets or built-in command
  lists changed;
- inspect runtime load errors with `/logs` or the current runtime log when a
  plugin does not appear;
- tell the user the command name, what each option/argument means, whether it is
  available as a model tool, and why they might want it.

When you identify an additional command that seems useful, say so explicitly:
describe the proposed command, when it would run, its options, and why it should
or should not be made durable now.
