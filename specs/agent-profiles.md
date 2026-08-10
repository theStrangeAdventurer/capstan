# Agent Profiles

Agent profiles are named workflow policies for Capstan runs. They are separate
from raw provider options: a profile can add system prompt guidance, set a
default reasoning effort, select a profile-specific model, and restrict model
tools.

## Profiles

| Profile | Purpose | Default reasoning | Tool policy |
|---------|---------|-------------------|-------------|
| `fast` | Low-overhead work for simple tasks | `low` | Normal tools |
| `implement` | Focused code changes with evidence-driven scoped-change discipline | `medium` | Normal tools |
| `plan` | Read-only exploration and planning | `high` | Inspection tools and read-only subagents |

`implement` tells the model to establish expected behavior from the relevant
test, specification, or caller; form a concrete hypothesis; run an early,
focused check when useful; and inspect failures before another speculative
change. It must stop broad exploration and validation after the required
behavior is evidenced.

`plan` keeps model-initiated `file_read`, `fetch`, `logs`, and `subagents`
tools. It removes write tools and shell from the model tool list, and the
runtime also rejects any model tool call that is not available in the active
profile before permissions or plugin handlers run. Plan subagents inherit the
plan profile and only receive a subset of the parent run's read-only tools; they
cannot spawn nested subagents. Manual slash commands remain direct user actions;
`/plan` does not prevent the user from typing another manual command.

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
command text to model history. In the TUI, Shift-Tab cycles profiles in this
order: `fast` -> `implement` -> `plan` -> `fast`.

## Precedence

Profile selection order:

1. `capstan run --profile ...`
2. TUI runtime state set by `/fast`, `/implement`, or `/plan`
3. `agent.profile` in `config.lua`
4. `implement`

Explicit `--reasoning-effort` takes precedence over a profile's default effort.
Provider-level reasoning configuration still applies when neither the run nor
the active profile set an effort.

Profile-specific model selection order for normal agent runs:

1. explicit run provider/model options such as `--provider` and `--model`
2. environment provider/model overrides
3. `/models --profile ...` persisted runtime state
4. `agent.profile_models` in `config.lua`
5. selected global primary provider/model

The TUI publishes the effective profile, provider/model, and reasoning effort
during startup, so the status line is visible before the first agent request.
The footer keeps the effort beside provider/model, and displays `default` when
no explicit effective effort is sent. When no profile is configured or
selected, `implement` is the real fallback profile:
its system prompt and default reasoning apply.

## Architecture

`agent/profiles.lua` owns profile definitions and model tool filtering.
`agent/runtime.lua` resolves the active profile before each run, appends the
profile prompt to the system message, applies default reasoning effort, and
filters tools after hook-based tool collection. `agent/tools.lua` refuses to
execute model tool calls that are absent from the filtered tool list and passes
the active profile into subagent runs.
`agent/models.lua` owns profile model persistence and effective-model reporting.
The profile slash commands are small embedded Lua plugins.

## Tests

- `make test` covers CLI profile parsing.
- `make test-http-lua` covers profile prompt injection, reasoning defaults,
  plan-mode tool filtering, subagent profile inheritance, and rejection of
  unavailable model tool calls.
- `make test-build` verifies the profile slash commands are embedded in the
  standalone binary.
