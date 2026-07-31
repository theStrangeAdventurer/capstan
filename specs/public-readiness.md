# Public Readiness

This spec tracks what Capstan can safely claim before public demos or launch
posts.

## Current Claims

- Capstan is a C TUI with an embedded Lua agent runtime.
- Built-in Lua runtime assets and core plugins are embedded into the binary.
- User plugins live as separate Lua files under `~/.config/capstan/plugins/`
  and are hot-reloaded by the running process.
- Model-initiated tool calls go through explicit permissions.
- Runtime logs and shell/tool continuations redact common secret shapes.
- Model-initiated file tools reject symlink escapes from the active workspace
  and prompt for sensitive local filenames such as `.env`.
- Blocking HTTP helpers enforce time and response-size limits.
- The `self-improvement` built-in skill is opt-in through
  `capabilities.self_improvement = true`.
- The binary can be smoke-tested with `./build/capstan --self-test-embedded`.

## Measured Claims

Only publish measurements that include the command or recorded methodology used
to produce them.

Useful local commands:

```sh
ls -lh build/capstan
file build/capstan
./build/capstan --self-test-embedded
make test
make test-http-lua
make test-build
```

Binary-size numbers are platform/build specific. Treat a local macOS size as a
local data point, not as a universal release guarantee.

The repository also contains [the Capstan vs opencode benchmark](../BENCHMARK_REPORT.md).
Public summaries may quote its exact results when they:

- identify it as the recorded 12-task Aider Polyglot mini-v2 benchmark with
  three runs per task and agent;
- keep the shared model/provider and environment context visible or linked;
- record the exact Capstan revision/worktree state and OpenCode version;
- link the report for commands, task-level results, artifacts, and limitations;
- avoid presenting the result as universal performance superiority.

## Not Yet Current Claims

Do not present these as implemented:

- forked subagents;
- isolated plugin sandboxing;
- sandboxed MCP servers;
- universal performance superiority outside recorded, reproducible measurements.

## Readiness Checklist

- README has build, run, config, permissions, plugin, and troubleshooting
  sections.
- Startup does not require manually exporting `TERMINFO` in common terminal,
  tmux, or screen setups.
- Specs and README use one config model: `~/.config/capstan/config.lua`.
- Test suite and standalone embedded smoke test pass before publishing.
