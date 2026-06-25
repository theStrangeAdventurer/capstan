# Public Readiness

This spec tracks what Capstan can safely claim before public demos or launch
posts.

## Current Claims

- Capstan is a C TUI with an embedded Lua agent runtime.
- Built-in Lua runtime assets and core plugins are embedded into the binary.
- User plugins live as separate Lua files under `~/.config/capstan/plugins/`
  and are hot-reloaded by the running process.
- Model-initiated tool calls go through explicit permissions.
- The `self-improvement` built-in skill is opt-in through
  `capabilities.self_improvement = true`.
- The binary can be smoke-tested with `./build/capstan --self-test-embedded`.

## Measured Claims

Only publish measurements that include the command used to produce them.

Useful commands:

```sh
ls -lh build/capstan
./build/capstan --self-test-embedded
make test
make test-http-lua
make test-build
```

Binary-size numbers are platform/build specific. Treat a local macOS size as a
local data point, not as a universal release guarantee.

## Not Yet Current Claims

Do not present these as implemented:

- headless batch mode;
- forked subagents;
- isolated plugin sandboxing;
- performance superiority beyond measured binary size and smoke-test behavior.

## Readiness Checklist

- README has build, run, config, permissions, plugin, and troubleshooting
  sections.
- Startup does not require manually exporting `TERMINFO` in common terminal,
  tmux, or screen setups.
- Specs and README use one config model: `~/.config/capstan/config.lua`.
- Test suite and standalone embedded smoke test pass before publishing.
