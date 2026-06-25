# Agent Hooks

Hooks let Lua config and plugins extend the agent pipeline without replacing the
whole `_G.on_messages` runtime.

## Behavior

Hooks are registered by stage name:

```lua
capstan.hooks.register("before_request", function(ctx)
  ctx.request.temperature = 0.2
  return ctx
end, {priority = 50})
```

Lower priority values run first. Equal priorities run in registration order. A
hook may mutate `ctx` in place and return nothing, or return a replacement ctx
table. Hook errors are caught, logged under the `hook` category, and the agent
cycle continues with the previous ctx value.

`config.lua` may declare hooks directly:

```lua
return {
  hooks = {
    before_request = function(ctx)
      ctx.request.metadata = {source = "config"}
      return ctx
    end,
  },
}
```

Plugins may declare hooks in the returned plugin table:

```lua
plugin.hooks = {
  after_tool_call = function(ctx)
    ctx.result = ctx.result .. "\n\npost-processed"
    return ctx
  end,
}
```

Plugins may be hook-only: `id` is required, while `command` and `handler` are
only needed for slash commands.

## Hook Stages

- `before_messages`: after system prompt and history are assembled. May change
  `ctx.messages`.
- `before_tools`: after model tools are collected from plugins. May change
  `ctx.tools`.
- `before_request`: before the request is encoded and sent. May change
  `ctx.request`, `ctx.headers`, and `ctx.endpoint`.
- `on_stream_chunk`: after a provider chunk is parsed, before it updates UI/tool
  accumulation. May change or clear `ctx.chunk`.
- `before_tool_call`: after JSON arguments are decoded, before permission check.
  May change `ctx.name`, `ctx.args`, `ctx.target`, and `ctx.permission_tool`.
- `after_tool_call`: after the plugin handler returns, before the tool result is
  appended to messages. May change `ctx.result`.
- `after_agent_turn`: after the final model stream completes without requesting
  more tool calls. This is the point where all tool continuations are done and
  control returns to the user. Receives `ctx.text`, `ctx.messages`,
  `ctx.tools`, `ctx.provider`, `ctx.provider_name`, and `ctx.runtime`.
- `after_subagents`: after all internal subagent runs complete and before the
  structured result is returned to the orchestrator as a tool result. May change
  `ctx.result`.

## Constraints

Hooks are policy and live in Lua. Full `_G.on_messages` replacement is not the
default extension mechanism.

`on_stream_chunk` is a hot-path hook. The runtime checks whether the stage has
registered hooks before allocating hook ctx for stream chunks.

`after_agent_turn` is the right stage for notifications that should fire only
when the user can respond again. Do not replace `_G.on_messages` for this.

## Tests

`make test-http-lua` covers config hooks, plugin hooks discovered before runtime
load, tool-call hooks, stream chunk hooks, final-turn hooks, and hook error
logging.
