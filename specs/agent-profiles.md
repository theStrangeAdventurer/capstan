# Agent Profiles

Profile policy is owned by `agent/profiles.lua`. Built-in definitions live in
`profiles/fast.lua`, `profiles/implement.lua`, and `profiles/plan.lua` and are
embedded in the standalone binary.

## User profiles

Capstan loads `~/.config/capstan/profiles/*.lua` in lexical filename order.
Each file returns one table:

```lua
-- ~/.config/capstan/profiles/review.lua
return {
  name = "review",
  label = "Review",
  order = 40,
  reasoning_effort = "high",
  readonly = true,
  completion_review = false,
  allowed_tools = {
    fetch = true,
    file_read = true,
    logs = true,
    subagents = true,
  },
  prompt = [[
## Active Profile: Review
Review the current changes without modifying files. Focus on correctness,
regressions, security, and missing tests. Report findings first, ordered by
severity, with precise file and line references when possible. If there are no
findings, say so explicitly and mention any remaining validation gaps.
]],
}
```

The profile is then available through Shift-Tab, `agent.profile = "review"`,
and `capstan run --profile review`. `allowed_tools` is an allowlist for
model-initiated tools; omitting it leaves all registered tools available.

A definition with an existing `name` patches that profile. Missing fields are
inherited, `prompt_append` appends instructions, and `replace = true` starts
from an empty definition. A profile may set `default = true`. Invalid files are
reported and skipped without removing the prior valid definition. Profile
prompts must be strings or arrays of strings, `prompt_append` must be a string,
and `allowed_tools` must map tool names to booleans. Project-local
Lua profiles are not auto-loaded because repository Lua is executable code.

Small patches may alternatively be placed under `agent.profiles` in
`config.lua`. User profile files and inline patches are both ignored by isolated
benchmark runs. Setting `default = false`, or replacing a profile without a
default flag, removes its default status. `agent.system_prompt_append` adds
global instructions before the active profile prompt. Profile order drives
Shift-Tab, ACP modes, and `/info`.

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
command text to model history. A TUI `--profile` option selects the initial
profile but does not pin it, so commands and Shift-Tab can change it. Shift-Tab cycles the complete registry by
ascending `order` (then name), including user profiles. With the review example
above, the built-in sequence continues from `plan` to `review` and then `fast`.

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
