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
capstan run --session-id "my fucking bench" --prompt-file task.md
capstan run --benchmark --prompt-file task.md --workdir /tmp/repo/task --workspace /tmp/repo
capstan run --no-wiki --prompt-file task.md --workdir /tmp/capstan-eval
echo "Summarize this repo" | capstan run --json
```

- `capstan` with no command still opens the TUI. `capstan --yolo` opens it with
  permission prompts disabled for all model tool calls in that process.
- `capstan run` reads input from `--prompt`, `--prompt-file`, or stdin when
  stdin is not a TTY.
- `--provider`, `--model`, `--workdir`, and `--workspace` override only this process run.
  They do not update runtime state.
- `--workdir` sets the base for relative file paths and shell commands.
  `--workspace` sets the containing project/instruction/permission root.
- `--session-id ID` persists the one-shot prompt and response as a new
  workspace-scoped session, pins both its ID and title to the exact supplied
  value, and isolates runtime logs beneath that ID. IDs may contain quoted
  spaces but not path separators, controls, leading dots, or edge spaces. An
  existing ID is an error; CLI mode never resumes or overwrites it implicitly.
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
- `--max-turns` caps model-tool continuation rounds; the default is `200`.
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
