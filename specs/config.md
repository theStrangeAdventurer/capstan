# Config

Capstan loads an optional unified Lua config from:

```text
~/.config/capstan/config.lua
```

The file must return a table. Missing config is normal and startup continues
with built-in defaults.

## Shape

```lua
return {
  provider = "openrouter",
  providers = {
    openrouter = { model = "minimax/minimax-m3" },
  },
  permissions = {
    { tool = "file_read", pattern = "/repo *", allow = true },
  },
  capabilities = {
    self_improvement = false,
  },
  finder = {
    ignore_files = { ".gitignore", ".ignore" },
    ignore_patterns = { "vendor/**", "build/**", "*.o" },
  },
  hooks = {
    before_request = function(ctx)
      ctx.request.temperature = 0.2
      return ctx
    end,
  },
}
```

## Behavior

- `provider` and `providers` configure the `agent/` Lua runtime.
- Environment variables still override config values for explicit runtime
  provider selection, API keys, model, and context limits.
- `permissions` entries define editable permission defaults. Runtime prompt
  choices are persisted separately in state and load after config permissions.
- `capabilities` contains explicit high-risk feature gates. Missing capability
  flags are treated as disabled.
- `finder.ignore_files` lists workspace-root ignore files to read in addition
  to the default `.gitignore`.
- `finder.ignore_patterns` adds gitignore-like patterns directly.
- `hooks` registers [agent hook](hooks.md) functions during runtime startup.
- `capabilities.self_improvement = true` explicitly enables the built-in
  [self-improvement skill](self-improvement.md). It is disabled by default.

## Compatibility

Existing `system_prompt.txt` remains supported.
`~/.config/capstan/providers.lua` is not loaded; provider settings belong in
the `provider` and `providers` sections of this unified config. `config.lua` is
the shared configuration entry point for editable defaults, not a replacement
for persisted [runtime state](runtime-state.md).
