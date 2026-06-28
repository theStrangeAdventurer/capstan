# CLI Run Mode

`capstan run` is the headless command-line entry point for one agent task. It
uses the same embedded Lua runtime, providers, plugins, hooks, and permissions
as the TUI.

## Behavior

```sh
capstan run --prompt "Inspect the build failure"
capstan run --prompt-file task.md --provider openrouter --model anthropic/claude-sonnet-4
capstan run --prompt-file task.md --reasoning-effort low
capstan run --profile plan --prompt-file task.md
capstan run --benchmark --prompt-file task.md --workdir /tmp/capstan-eval
echo "Summarize this repo" | capstan run --json
```

- `capstan` with no command still opens the TUI.
- `capstan run` reads input from `--prompt`, `--prompt-file`, or stdin when
  stdin is not a TTY.
- `--provider`, `--model`, and `--workdir` override only this process run.
  They do not update runtime state.
- `--profile` selects an agent workflow profile for this run. Accepted values
  are `fast`, `implement`, and `plan`. Profiles can set default reasoning
  effort, append profile-specific system instructions, and restrict available
  model tools. `plan` is read-only for model tools: it keeps inspection tools
  such as `file_read`, `fetch`, and `logs`, and removes write, shell, and
  subagent tools.
- `--reasoning-effort` overrides the run's reasoning effort. Accepted values
  are `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, and `max`.
  `--effort` is a short alias. The runtime maps this into request-level
  `reasoning.effort` for compatible providers. Explicit `--reasoning-effort`
  takes precedence over profile defaults.
- `--max-turns` caps model-tool continuation rounds; the default is `200`.
- `--no-mcp` skips configured MCP server startup for this process run.
- `--full-control` grants workspace-scoped tool permissions for this run.
- `--benchmark` is shorthand for `--no-mcp --full-control`.
- Plain output writes the final assistant text to stdout.
- `--json` writes `{ ok, text, error }` to stdout.

## Architecture

The C entry point parses CLI args, initializes the normal embedded runtime
without ncurses, calls `capstan.agent.run`, and drives `http_poll()` until the
Lua run reports completion. Blocking HTTP helpers run in headless mode and do
not call TUI rendering functions.

`_G.agent_entry` remains the TUI adapter. Both TUI and CLI call the same
`capstan.agent.run(opts, callbacks)` runtime function.

## Tests

`make test` covers CLI option parsing. `make test-build` covers embedded runtime
availability from an isolated directory.
