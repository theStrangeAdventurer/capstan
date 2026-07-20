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
  agent = {
    profile = "implement",
    profile_models = {
      fast = { provider = "openrouter", model = "cheap/fast" },
      plan = { provider = "openrouter", model = "strong/planner" },
      implement = { provider = "openrouter", model = "strong/coder" },
    },
    reasoning_effort = "medium",
    max_turns = 80,
    max_duration_sec = 900,
    stream_timeout_sec = 300,
    max_stream_retries = 1,
    max_tool_calls = 80,
    max_same_tool_call = 3,
    max_same_shell_command = 0,
    max_generated_output_checks = 1,
    completion_review = true,
  },
  subagents = {
    max_concurrent = 3,
    max_tasks = 8,
    max_turns = 6,
    max_turns_cap = 200,
    max_result_bytes = 16384,
  },
  finder = {
    ignore_files = { ".gitignore", ".ignore" },
    ignore_patterns = { "vendor/**", "build/**", "*.o" },
  },
  redaction = {
    names = { "tenant-id" },
    name_patterns = { "^x%-internal%-" },
    value_patterns = { "org_[%w%-]+" },
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
- `capabilities` contains explicit feature gates. Missing `subagents` is treated
  as enabled; high-risk capabilities such as `self_improvement` remain disabled
  when missing.
- `finder.ignore_files` lists workspace-root ignore files to read in addition
  to the default `.gitignore`.
- `finder.ignore_patterns` adds gitignore-like patterns directly.
- `redaction` extends secret masking in Lua-visible shell output, tool results,
  and runtime logs. Built-in masking for common credentials remains enabled;
  config entries only add project-specific rules.
- `hooks` registers [agent hook](hooks.md) functions during runtime startup.
- `capabilities.self_improvement = true` explicitly enables the built-in
  [self-improvement skill](self-improvement.md). It is disabled by default.
- `capabilities.subagents = false` hides the model tool described in
  [Subagents](subagents.md). It is enabled by default when the field is omitted.
- `agent.max_turns`, `max_duration_sec`, `stream_timeout_sec`,
  `max_stream_retries`, `max_tool_calls`,
  `max_same_tool_call`, `max_same_shell_command`, and
  `max_generated_output_checks` limit runaway agent/tool loops. Missing values
  fall back to the built-in defaults.
  `max_same_shell_command = 0` disables the shell-specific repeated-command
  guard. `max_generated_output_checks = 0` disables the soft generated-output
  inspection limit; its default is one inspection per agent run.
  `stream_timeout_sec` bounds one streaming model request (zero disables this
  per-request limit); `max_stream_retries` retries a transient transport or
  server failure only before it has emitted text, so a stalled transport cannot
  leave an agent run waiting indefinitely or duplicate a visible answer. Their
  defaults are 300 seconds and one retry.
- `agent.completion_review` controls one final, bounded review pass after a
  meaningful implementation phase. It defaults to enabled for the `implement`
  profile and disabled for `fast` and `plan`. A phase is meaningful when it
  changes more than one file, or when a successful project validation follows a
  workspace write. The review shares the same conversation and tools, runs at
  most once per root agent run, and is not run for subagents.
- `agent.profile` sets the default workflow profile. Accepted values are
  `fast`, `implement`, and `plan`. Profiles may append system instructions,
  set a default reasoning effort, and restrict model tools. Slash commands
  `/fast`, `/implement`, and `/plan` change the profile for the current TUI
  session; `capstan run --profile ...` overrides it for one headless run. When
  omitted, Capstan uses the `implement` profile.
- `agent.profile_models` can set default provider/model pairs per workflow
  profile. A profile model is used for normal agent runs when that profile is
  active, without changing the global primary model. `weak_model` remains a
  separate background/compact model.
- `agent.reasoning_effort` sets the default effort policy for agent runs.
  Accepted values are `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, and
  `max`. Run options such as `capstan run --reasoning-effort low` override this
  for one process. Explicit reasoning effort takes precedence over profile
  defaults.
- Provider entries may set `reasoning = { ... }`, `reasoning_effort`,
  `reasoning_max_tokens`, or `reasoning_exclude`; these are copied into the
  OpenAI-compatible request body as a `reasoning` object. The run-level effort
  override takes precedence over provider defaults.
- `subagents.max_concurrent`, `max_tasks`, `max_turns`, `max_turns_cap`, and
  `max_result_bytes`
  limit delegated internal runs. `max_turns` is the default per-task turn budget
  when a subagent task omits its own value; explicit task budgets are respected
  up to `max_turns_cap`. `max_result_bytes` bounds each successful child result
  before it is inserted into the parent model context; failed child output is
  discarded and only a short single-line error is retained.
- `redaction.names` is a case-insensitive list of field or header names whose
  values should be masked.
- `redaction.name_patterns` is a list of Lua patterns matched against the
  lowercased field or header name.
- `redaction.value_patterns` is a list of Lua patterns replaced anywhere in
  text with `[REDACTED]`.

## Compatibility

Existing `system_prompt.txt` remains supported.
`~/.config/capstan/providers.lua` is not loaded; provider settings belong in
the `provider` and `providers` sections of this unified config. `config.lua` is
the shared configuration entry point for editable defaults, not a replacement
for persisted [runtime state](runtime-state.md).
