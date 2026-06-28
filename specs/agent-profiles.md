# Agent Profiles

Agent profiles are named workflow policies for Capstan runs. They are separate
from raw provider options: a profile can add system prompt guidance, set a
default reasoning effort, and restrict the model tool list.

## Profiles

| Profile | Purpose | Default reasoning | Tool policy |
|---------|---------|-------------------|-------------|
| `fast` | Low-overhead work for simple tasks | `low` | Normal tools |
| `implement` | Focused code changes | `medium` | Normal tools |
| `plan` | Read-only exploration and planning | `high` | Inspection tools only |

`plan` keeps model-initiated `file_read`, `fetch`, and `logs` tools. It removes
write tools, shell, and subagents from the model tool list. Manual slash
commands remain direct user actions; `/plan` does not prevent the user from
typing another manual command.

## Selection

Headless runs use:

```sh
capstan run --profile plan --prompt-file task.md
```

The TUI exposes no-history control commands:

```text
/fast
/implement
/plan
```

These commands update the active profile for later TUI turns without adding the
command text to model history.

## Precedence

Profile selection order:

1. `capstan run --profile ...`
2. TUI runtime state set by `/fast`, `/implement`, or `/plan`
3. `agent.profile` in `config.lua`
4. No active profile

Explicit `--reasoning-effort` takes precedence over a profile's default effort.
Provider-level reasoning configuration still applies when neither the run nor
the active profile set an effort.

## Architecture

`agent/profiles.lua` owns profile definitions and model tool filtering.
`agent/runtime.lua` resolves the active profile before each run, appends the
profile prompt to the system message, applies default reasoning effort, and
filters tools after hook-based tool collection. The profile slash commands are
small embedded Lua plugins.

## Tests

- `make test` covers CLI profile parsing.
- `make test-http-lua` covers profile prompt injection, reasoning defaults, and
  plan-mode tool filtering.
- `make test-build` verifies the profile slash commands are embedded in the
  standalone binary.
