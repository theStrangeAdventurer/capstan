# CLI Run Mode

`capstan run` is the headless command-line entry point for one agent task. It
uses the same embedded Lua runtime, providers, plugins, hooks, and permissions
as the TUI.

## Behavior

```sh
capstan --yolo --provider deepseek --model deepseek-chat --profile implement --effort medium
capstan --workdir /tmp/repo/task --workspace /tmp/repo --no-mcp --no-wiki
capstan --session-id "release prep"
capstan run --prompt "Inspect the build failure"
capstan run --prompt-file task.md --provider openrouter --model anthropic/claude-sonnet-4
capstan run --prompt-file task.md --reasoning-effort low
capstan run --profile plan --prompt-file task.md
capstan run --session-id "my benchmark" --prompt-file task.md
capstan run --session-id "my benchmark" --prompt "Continue the analysis"
capstan run --benchmark --prompt-file task.md --workdir /tmp/repo/task --workspace /tmp/repo
capstan run --no-wiki --prompt-file task.md --workdir /tmp/capstan-eval
echo "Summarize this repo" | capstan run --json
```

- `capstan` with no command opens the TUI. Interactive mode accepts
  `--provider`, `--model`, `--profile`, `--reasoning-effort`/`--effort`,
  `--workdir`, `--workspace`, `--session-id`, `--max-turns`, `--no-mcp`,
  `--no-wiki`, `--no-preserve-reasoning`, and
  `--yolo` in any combination. Runtime overrides apply only to the current
  process; session selection updates the workspace's active session.
- `--prompt`, `--prompt-file`, `--json`, and `--benchmark` are headless-only
  and require `capstan run`.
- `capstan --yolo` opens the TUI with permission prompts disabled for all model
  tool calls in that process.
- `capstan run` reads input from `--prompt`, `--prompt-file`, or stdin when
  stdin is not a TTY.
- `--provider`, `--model`, `--workdir`, and `--workspace` override only this process run.
  They do not update runtime state.
- `--workdir` sets the base for relative file paths and shell commands.
  `--workspace` sets the containing project/instruction/permission root.
- `--session-id ID` selects a workspace-scoped session. If it exists, TUI
  restores it as the active conversation and `capstan run` sends the stored
  history plus the new prompt. If it does not exist, Capstan creates it with a
  stable ID and explicit title matching the supplied value, then uses it
  immediately. The flag also isolates runtime logs beneath that stable ID.
  IDs may contain quoted spaces and UTF-8 text, but not path separators,
  controls, leading dots, or edge spaces. Invalid or malformed existing
  sessions fail closed without being overwritten.
- `--profile` selects an agent workflow profile for this run. Accepted values
  are `fast`, `implement`, and `plan`. Profiles can set default reasoning
  effort, append profile-specific system instructions, and restrict available
  model tools. `plan` is read-only for model tools: it keeps inspection tools
  such as `file_read`, `fetch`, `logs`, and `subagents`, and removes write and
  shell tools. Plan subagents inherit the read-only profile and cannot spawn
  nested subagents.
- `--reasoning-effort` overrides the run's reasoning effort. Accepted values
  are `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, and `max`.
  `--effort` is a short alias. The runtime maps this into request-level
  `reasoning.effort` for compatible providers. Explicit `--reasoning-effort`
  takes precedence over profile defaults.
- `--max-turns` caps model-tool continuation rounds; the default is `200` in
  headless runs and takes precedence over the `agent.max_turns` config value,
  which governs interactive runs (built-in default `80`).
- `--no-mcp` skips configured MCP server startup for this process run.
- `--no-wiki` skips wiki initialization for this process run. Capstan does not
  create the default wiki directory, read wiki files, add wiki metadata or
  profile content to the system prompt, publish wiki runtime fields, or expose
  wiki model tools. This is intended for isolated eval runs that must not
  depend on owner wiki state.
- `--no-preserve-reasoning` disables returning provider reasoning blocks with
  assistant tool calls for this run. Preservation is enabled by default so a
  reasoning model can continue an interrupted tool-use turn. This diagnostic
  override can degrade continuity or trigger a provider protocol error.
- `--yolo` automatically allows permission decisions except explicit denies
  for this process.
- `--benchmark` enables `--no-mcp --no-wiki`, an internal workspace-only
  permission scope, and an isolated runtime. It uses the embedded base prompt
  and built-in tools, but excludes a
  local system-prompt override, project/user/common skills, `AGENTS.md`, global
  config plugins, and config/plugin hooks so eval tasks cannot inherit unrelated
  owner or repository policy.
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

`make test` covers CLI option parsing and named-session policy.
`make test-http-lua` covers session-scoped logging. `make test-build` covers
embedded runtime availability from an isolated directory.
