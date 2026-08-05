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
    openrouter = {
      model = "minimax/minimax-m3",
      default_reasoning_efforts = { "low", "medium", "high" },
    },
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
    max_duration_sec = 2700,
    stream_timeout_sec = 300,
    max_stream_retries = 1,
    max_tool_calls = 80,
    max_same_tool_call = 3,
    max_same_shell_command = 0,
    max_generated_output_checks = 1,
    completion_review = true,
    auto_compact_percent = 80,
  },
  subagents = {
    max_concurrent = 3,
    max_tasks = 8,
    max_turns = 6,
    max_turns_cap = 200,
    max_result_bytes = 16384,
  },
  tool_output = {
    max_bytes = 50 * 1024,
    max_lines = 2000,
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
- `tool_output.max_bytes` and `tool_output.max_lines` bound each tool result
  inserted into provider continuation history. Defaults are 50 KiB and 2,000
  lines. The visible truncation marker includes the original byte count and
  directs the model toward a narrower query or paged read.
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
  multi-file implementation phase without successful validation. It defaults
  to enabled for the `implement` profile and disabled for `fast` and `plan`.
  Successful validation suppresses the redundant pass until another workspace
  write invalidates that evidence. The review shares the same conversation and
  tools, runs at most once per root agent run, and is not run for subagents.
- `agent.auto_compact_percent` controls automatic conversation compaction in
  the interactive TUI. Before an ordinary submission, Capstan estimates the
  complete next prompt, including the system/profile instructions, current
  history, new user text, and available tool schemas. At or above the threshold
  it compacts the existing history first and keeps the new submission in the
  normal FIFO. The default is `80`; `0` disables automatic compacting. Values
  above `100` are treated as `100`. Auto-compact is skipped when the active
  model's context limit is unknown.
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
- `agent.preserve_reasoning` defaults to `true`. When enabled, Capstan returns
  provider-native `reasoning_details` with assistant tool calls, falling back
  to plaintext `reasoning` only when structured details are absent. Set it to
  `false`, or use `capstan run --no-preserve-reasoning`, to omit that context.
  Disabling it is diagnostic: providers that require signed, encrypted, or
  plaintext reasoning continuity may reject the next request or repeat work.
- Provider entries may set `reasoning = { ... }`, `reasoning_effort`,
  `reasoning_max_tokens`, or `reasoning_exclude`; these normally form the
  OpenAI-compatible `reasoning` request object. `reasoning_effort_field` moves
  the effort to a provider-specific top-level field such as direct DeepSeek's
  `reasoning_effort`. The run-level effort override takes precedence over
  provider defaults.
- `reasoning_efforts[model_id]` declares the ordered effort choices for a known
  model. For model APIs that expose `supported_parameters`, the provider's
  `default_reasoning_efforts` supplies the ordered choices after reasoning
  support is detected. These fields drive the mandatory second `/models`
  selection step; they are capability metadata, not prompt instructions.
- A direct chat-completions provider that requires plaintext history under
  `reasoning_content` may set `reasoning_history_field = "reasoning_content"`;
  other providers default to `reasoning` when no structured details are
  returned.
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
