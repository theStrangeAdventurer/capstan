---
name: self-improvement
description: Use only when a task requires extending Capstan itself with user plugins, hooks, or local runtime automation.
---

# Self Improvement

Use this skill only when the user's task benefits from teaching Capstan a new
local behavior that should remain available after the current request, such as a
slash command, model tool, autocomplete source, or agent hook.

This skill is available only when the user has explicitly enabled it in
`~/.config/capstan/config.lua`:

```lua
return {
  capabilities = {
    self_improvement = true,
  },
}
```

## Where To Write Extensions

Write user extensions as separate Lua files under:

```text
~/.config/capstan/plugins/*.lua
```

Do not store plugin source code inside the shared config table. The running
Capstan process watches the plugin directory and hot-reloads new, changed, and
deleted `.lua` files.

## Plugin Shape

A plugin file returns a table:

```lua
local plugin = {
  id = "my-plugin",
  name = "My Plugin",
  description = "Short user-facing description.",
}

return plugin
```

For slash commands, add `command` and `handler`:

```lua
plugin.command = "/mine"
plugin.handler = function(ctx)
  return "Shown in chat", "Sent to the model"
end
```

For runtime-only commands that should not trigger an agent request, set:

```lua
plugin.history = false
```

## Hooks

Hook-only plugins may omit `command` and `handler` and define:

```lua
plugin.hooks = {
  before_request = function(ctx)
    return ctx
  end,
}
```

Available hook points are documented in `specs/hooks.md`: `before_messages`,
`before_tools`, `before_request`, `on_stream_chunk`, `before_tool_call`, and
`after_tool_call`.

## Rules

- Keep extensions narrow and reversible.
- Prefer a plugin or hook over editing Capstan core when runtime extension is
  enough.
- Do not create background network calls or shell execution unless the user task
  clearly requires it.
- Include clear plugin ids; changing an id creates a different plugin.
- If a plugin writes files, uses shell commands, or changes agent behavior in a
  durable way, make that behavior visible in the final response.
- If the extension fails to load, keep the previous working version intact and
  fix the file rather than deleting user work.
