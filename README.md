# Capstan

Capstan is a compact terminal coding agent built in C with an embedded Lua
agent runtime. It combines a native ncurses TUI, headless automation, explicit
tool permissions, and an extension model based on plain Lua and Markdown.

Capstan is designed to stay local, inspectable, fast to start, and easy to
extend without rebuilding the binary.

## Highlights

- **Compact native executable.** ncurses, Lua, the agent runtime, and built-in
  plugins are embedded into one executable. A current local Apple Silicon
  macOS build is 1,160,872 bytes (about 1.1 MiB).
- **Interactive and headless.** Use the full terminal UI or run one-shot tasks
  with `capstan run`, including structured JSON output for scripts and CI.
- **Controlled autonomy.** Model tools pass through profile availability,
  workspace boundaries, sensitive-file checks, and explicit permission rules.
- **Three workflow profiles.** `fast`, `implement`, and a model-tool read-only
  `plan` profile can use different models and reasoning effort.
- **Persistent project sessions.** TUI conversations, generated titles, prompt
  history, and explicitly named headless runs are stored per workspace.
- **Extensible without recompilation.** Lua plugins can add slash commands,
  autocomplete, model tools, and lifecycle hooks. TUI plugin changes are
  hot-reloaded.
- **Open local context.** Capstan loads project `AGENTS.md`, discovers reusable
  skills, and provides a portable Markdown Wiki instead of opaque hidden
  memory.
- **MCP and multimodal tools.** Connect stdio or Streamable HTTP MCP servers and
  pass validated local or MCP-provided images to vision-capable models.
- **Parallel subagents.** The orchestrator can run several focused internal
  agent tasks concurrently with bounded tools, turns, and result sizes.

## Quick Start

Build and start with the default DeepSeek provider:

```sh
./build.sh
DEEPSEEK_API_KEY=... ./build/capstan
```

Or use OpenRouter:

```sh
OPENROUTER_API_KEY=... AI_PROVIDER=openrouter ./build/capstan
```

Headless one-shot mode:

```sh
./build/capstan run --prompt "Inspect this repository"
./build/capstan run --profile plan --prompt-file task.md
./build/capstan run --prompt-file task.md --json
```

DeepSeek and OpenRouter are built in. Other OpenAI-compatible providers can be
added in `~/.config/capstan/config.lua`.

## Benchmark: Capstan vs OpenCode

The canonical Aider Polyglot `mini-v2` suite was run three times per agent on
Apple Silicon macOS: 12 fixed tasks per run across C++, Go, Java, JavaScript,
Python, and Rust. Both agents used the direct DeepSeek provider, V4 Pro with
medium reasoning, a 240-second per-task timeout, sequential execution, and no
Promptfoo cache.

| Agent | Canonical passes | Median agent time | Total agent time |
|---|---:|---:|---:|
| **Capstan** (`b7a664ef`, dirty worktree) | **36/36 (100%)** | **64.3s** | **53m 19s** |
| OpenCode 1.18.2 | 35/36 (97.2%) | 79.0s | 58m 09s |

Capstan completed every attempt, led by one canonical pass, used 18.7% less
median agent time, and finished its 36 agent runs 4m 50s sooner. In a separate
15-run local runtime-footprint benchmark, Capstan's median peak RSS was
14.7 MiB versus 555.0 MiB for OpenCode (37.8x lower), and median local CPU time
was 0.19s versus 2.97s (15.6x lower). The footprint test used the same model but
a smaller three-task workload, so it is reported separately rather than mixed
into the Polyglot score.

See the [standalone visual report](BENCHMARK_REPORT.html) for all 72 Polyglot
attempts, resource charts, methodology, and limitations, or read the
[Markdown report](BENCHMARK_REPORT.md). Run a comparable update with the
checked-in [benchmark tooling](benchmarks/README.md); it clones the pinned
external corpus locally and keeps generated results out of Git. The historical
quality result itself used a dirty Capstan worktree, as documented in the report.

## Install

Prebuilt binaries are available for Apple Silicon macOS, x86-64 Linux, and
ARM64 Linux. The installer detects the platform, downloads the latest release,
verifies its SHA-256 checksum, and installs `capstan` into `~/.local/bin`:

```sh
curl -fsSL https://raw.githubusercontent.com/theStrangeAdventurer/capstan/main/install.sh | sh
```

If `~/.local/bin` is not already on `PATH`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Choose another install directory:

```sh
curl -fsSL https://raw.githubusercontent.com/theStrangeAdventurer/capstan/main/install.sh |
  CAPSTAN_INSTALL_DIR="$HOME/bin" sh
```

Install a specific release:

```sh
curl -fsSL https://raw.githubusercontent.com/theStrangeAdventurer/capstan/main/install.sh |
  CAPSTAN_VERSION=v0.1.0 sh
```

For manual installation, download the archive from
[GitHub Releases](https://github.com/theStrangeAdventurer/capstan/releases),
verify it against `SHA256SUMS`, extract it, and move `capstan` to a directory on
your `PATH`. Release archives also contain `LICENSE`, `NOTICE`, and
`THIRD_PARTY_NOTICES`.

## Build From Source

```sh
./build.sh
```

The script checks system dependencies, builds vendored ncurses and Lua, embeds
the Lua runtime and built-in plugins, and writes `build/capstan`.

Binary size is platform- and build-specific. Measure your build with:

```sh
ls -lh build/capstan
file build/capstan
```

### System Dependencies

macOS:

```sh
xcode-select --install
```

Debian/Ubuntu:

```sh
sudo apt install build-essential libcurl4-openssl-dev ncurses-bin
```

Fedora:

```sh
sudo dnf install gcc make libcurl-devel ncurses
```

ncurses and Lua are linked from vendored static archives. `libcurl` is the only
non-system runtime library expected from the host; the executable still uses
the platform's normal system libraries.

## Configuration

Capstan loads:

```text
~/.config/capstan/config.lua
```

Example:

```lua
return {
  provider = "openrouter",
  providers = {
    openrouter = {
      endpoint = "https://openrouter.ai/api/v1/chat/completions",
      api_key = os.getenv("OPENROUTER_API_KEY"),
      model = "anthropic/claude-sonnet-4",
    },
  },
  agent = {
    profile = "implement",
    profile_models = {
      fast = {provider = "openrouter", model = "minimax/minimax-m3"},
      plan = {provider = "openrouter", model = "anthropic/claude-sonnet-4"},
    },
  },
  permissions = {
    {tool = "file_read", pattern = "~/code/project/*", allow = true},
    {tool = "fetch", pattern = "https://api.github.com/*", allow = true},
  },
  capabilities = {
    self_improvement = false,
    subagents = true,
  },
}
```

Environment variables and explicit run flags take precedence where supported.
Model choices made through `/models` are persisted as runtime state rather than
rewritten into `config.lua`.

### Agent Limits And Subagents

A **turn** is one model response. One response may contain several tool calls;
a later response that receives their results consumes the next turn. Configure
root-agent and delegated-agent limits independently:

```lua
return {
  agent = {
    max_turns = 80, -- root agent; positive values only
    max_duration_sec = 2700,
    max_tool_calls = 0,          -- 0 disables this guard
    max_same_tool_call = 0,      -- 0 disables this guard
    max_same_shell_command = 0,  -- 0 disables this guard
  },
  subagents = {
    max_concurrent = 3,      -- default child batch concurrency
    max_concurrent_cap = 4,  -- upper bound for a model-requested concurrency
    max_tasks = 8,           -- maximum tasks in one subagents call
    max_turns = 6,           -- default turns per child task
    max_turns_cap = 24,      -- upper bound for a model-requested child budget
    max_attempts = 3,        -- total transient-request attempts per child
  },
}
```

`subagents.max_turns_cap` is the upper bound you asked about: if the
orchestrator requests `max_turns = 200` for a child, the child receives at most
the configured cap (24 in this example). Omitted child budgets use
`subagents.max_turns`. The built-in defaults are 6 turns per child and a cap of
200. `capstan run --max-turns N` overrides the root run only; child budgets
continue to use the `subagents` configuration. See [configuration details](specs/config.md)
and [subagent behavior](specs/subagents.md).

### Permissions And `--yolo`

Permissions apply to model-initiated tools; slash commands you type yourself
are direct user actions. Add durable allow or deny rules to the same config:

```lua
return {
  permissions = {
    { tool = "file_read", pattern = "~/code/project/*", allow = true },
    { tool = "file_write", pattern = "~/code/project/*", allow = true },
    { tool = "fetch", pattern = "https://api.github.com/*", allow = true },
    -- Put specific denies after broad allows: later matching rules win.
    { tool = "file_read", pattern = "~/code/project/.env*", allow = false },
  },
}
```

Rules use `tool`, glob-style `pattern`, and `allow`. An explicit deny always
wins over `--yolo`; use it to protect sensitive files. Start a trusted local
session without confirmation prompts with:

```sh
capstan --yolo
capstan run --yolo --prompt "Run the project test suite"
```

`--yolo` auto-allows only otherwise-`ask` model tool calls for that process. It
does not persist permissions and should be used only in a trusted workspace.
For matching, session grants, sensitive-file handling, and the default policy,
see [Permissions](specs/permissions.md).

## Agent Profiles

| Profile | Purpose | Default reasoning | Model-tool policy |
|---|---|---|---|
| `fast` | Low-overhead simple work | `low` | Normal tools |
| `implement` | Focused code changes | `medium` | Normal tools |
| `plan` | Exploration and planning | `high` | Read-only inspection tools |

Use `/fast`, `/implement`, or `/plan` in the TUI, cycle profiles with
Shift-Tab, or pass `--profile` to `capstan run`.

The `plan` restriction is enforced in the runtime: write and shell tools are
removed from the model's available tool list, and unavailable calls are
rejected before plugin or permission handling. Manual slash commands remain
direct user actions and are not disabled by the profile.

## Sessions And Context

The TUI automatically saves and restores conversations per workspace:

- `/new` creates a new session;
- `/sessions` opens the searchable session list;
- a weak model can generate concise session titles in the background;
- prompt-entry history persists independently of conversation history;
- `/compact` replaces a long conversation with a compact operational handoff,
  and the TUI automatically does the same before an ordinary submission reaches
  80% of the active model context (`agent.auto_compact_percent`, `0` disables);
- ordinary messages submitted during an active run enter a bounded FIFO queue.

Session files are written atomically under the XDG state directory with private
Unix permissions. Headless runs remain ephemeral by default; pass
`--session-id "my bench"` to persist a run under an immutable title and write
its runtime events to a session-specific log directory.

## Project Instructions, Skills, And Wiki

Capstan automatically loads `AGENTS.md` from the workspace root.

Reusable skills are discovered from project, shared-home, user, and gated
built-in skill directories. Only compact FrontMatter metadata is placed in the
system prompt; the full `SKILL.md` is read only when the task matches it.

Capstan Wiki is a portable, inspectable Markdown knowledge directory:

- `profile/core.md` can provide a small stable owner profile;
- larger documents are exposed through a metadata-only index;
- full documents are read only when relevant;
- external Markdown roots can be indexed without modifying or copying them;
- copy mode can materialize selected Markdown into the Wiki;
- the directory can be edited normally and versioned with Git.

Use `/wiki` for status and onboarding, `/wiki read <path>` to read a document,
and `/wiki ingest [--copy] <path>` to index a Markdown source.

## MCP, Images, And Subagents

Capstan acts as an MCP client for configured external servers:

- local servers over stdio;
- remote servers over Streamable HTTP;
- background tool discovery so TUI startup is not blocked;
- per-tool permission checks and `/mcp` status/restart controls.

The current MCP client exposes tools; MCP resources, prompts, and sampling are
not supported yet. MCP servers are trusted integrations and are not sandboxed.

Local PNG, JPEG, GIF, and WebP files, plus validated MCP image results, can be
passed to vision-capable OpenAI-compatible models. In the TUI, press `Ctrl+V`
to attach an image from the system clipboard directly to the current prompt.
Formats are checked or normalized, decoded images are limited to 10 MiB each,
and image base64 is not written to runtime logs.

The `subagents` model tool can run focused internal tasks concurrently. Child
agents can receive narrower model, turn, concurrency, and tool limits. Network
waits are parallel; Lua callbacks and tool handlers remain single-threaded.

## Permissions And Safety

Permissioned model-initiated tools go through Capstan's permission policy.
Rules may allow, deny, or ask. The TUI offers allow once, allow the exact target
for the current session, allow the tool for every target in the session, and
reject. ACP exposes its client-defined equivalents: allow once, allow the exact
target for the session, and reject.

Additional safeguards include:

- normalized workspace boundaries for file and shell tools;
- real-path checks that reject symlink escapes;
- extra prompts for sensitive names such as `.env`, `secret`, `token`, and
  `credential` unless explicitly allowed;
- exact-fragment matching for file edits;
- bounded HTTP responses, image payloads, session rows, and subagent results;
- redaction of common secret shapes from logs and model-visible continuations;
- tool, turn, retry, and timeout limits.

Manual slash commands are treated as direct user intent and do not show the
model-tool permission prompt. Wiki reads are constrained to Capstan's Wiki
root; external Wiki ingest requires explicit file-read consent.

Session permission grants are not persisted. Permanent owner rules belong in
`~/.config/capstan/config.lua`. Explicit runtime workflows and older Capstan
versions may still have rules in
`$XDG_STATE_HOME/capstan/permissions.lua` (or
`~/.local/state/capstan/permissions.lua`).

## Built-In Commands

Type `/` and press Tab in the TUI to browse available commands.

- Files and execution: `/file`, `/write`, `/edit`, `/shell`, `/fetch`
- Sessions and context: `/new`, `/sessions`, `/compact`, `/editor`
- Profiles and models: `/fast`, `/implement`, `/plan`, `/models`
- Context and integrations: `/skills`, `/wiki`, `/mcp`
- Diagnostics: `/info`, `/logs`
- Plugin-provided OAuth flows: `/connect`, `/auth`, `/logout`

OAuth commands operate on providers supplied by compatible plugins; Capstan
does not promise a built-in OAuth provider.

## CLI Run Mode

`capstan run` uses the same embedded agent runtime, providers, built-in tools,
plugins, hooks, profiles, and permission policy as the TUI.

```sh
capstan run --prompt "Inspect the build failure"
capstan run --prompt-file task.md --provider openrouter --model MODEL
capstan run --profile plan --reasoning-effort high --prompt-file task.md
capstan run --prompt-file task.md --workdir ./src --workspace . --json
capstan run --benchmark --prompt-file task.md --workdir /tmp/task
```

Notable options:

- `--provider`, `--model`, `--profile`, `--reasoning-effort`
- `--workdir`, `--workspace`, `--max-turns`
- `--session-id` for a persisted immutable CLI session and isolated logs
- `--no-mcp`, `--no-wiki`, `--no-preserve-reasoning`
- `--yolo` to auto-allow permission prompts except explicit denies
- `--benchmark` for isolated workspace-scoped eval mode
- `--json` for `{ok, text, error}` output

`--benchmark` implies `--no-mcp --no-wiki`, uses an internal workspace-only
permission scope, and excludes local prompt overrides, external skills,
`AGENTS.md`, global plugins, and hooks so
evaluations do not inherit unrelated machine or repository policy.

Run `capstan --help` for the current option list.

## Plugins And Hooks

User plugins live at:

```text
~/.config/capstan/plugins/*.lua
```

In the TUI, new, changed, and deleted plugins are hot-reloaded. Headless
`capstan run` loads plugins once at startup.

Plugins can provide slash commands, autocomplete, model tools, OAuth adapters,
and hooks around messages, requests, stream chunks, tool calls, final agent
turns, and subagent completion.

Minimal command plugin:

```lua
local plugin = {
  id = "hello",
  name = "Hello",
  command = "/hello",
}

function plugin.handler(ctx)
  return ctx:replace("hello from plugin")
end

return plugin
```

The built-in `self-improvement` skill can create durable local automation but is
disabled by default. Enable it explicitly only if you want that high-risk
capability:

```lua
return {
  capabilities = {
    self_improvement = true,
  },
}
```

## Terminal Compatibility

Capstan builds ncurses with fallback descriptions for `xterm-256color`,
`tmux-256color`, `screen-256color`, `xterm`, `screen`, `ansi`, and `vt100`.
Bracketed paste is supported, so pasted newlines stay in the input editor.

Ordinary terminal, tmux, and screen setups should not require manual `TERMINFO`
configuration. If startup fails, Capstan prints the current `TERM` and an
actionable diagnostic before opening the TUI.

## Known Limitations

- User Lua plugins and configured MCP servers are trusted code/integrations, not
  sandboxed isolates.
- Blocking Lua plugin work can block UI cancellation until it returns.
- Subagent network waits are parallel, but Lua callbacks and tool handlers run
  on one Lua thread.
- MCP currently supports tools, not resources, prompts, sampling, or a separate
  long-lived server-to-client stream.
- Vision input depends on provider and model support.
- Benchmark results are workload- and environment-specific.

## Project Layout

```text
src/        C runtime, TUI, permissions, storage, process and HTTP bridges
include/    C headers
agent/      embedded Lua agent runtime
plugins/    embedded built-in Lua plugins
skills/     gated built-in skills
specs/      behavior and architecture specifications
test/       C and Lua tests
vendor/     vendored ncurses, Lua, munit, and Lua JSON
```

## Development Checks

```sh
make test
make test-http-lua
make test-build
```

Smoke-test only the embedded runtime without opening the TUI:

```sh
./build/capstan --self-test-embedded
```

## License

Capstan is licensed under the Apache License 2.0. See [LICENSE](LICENSE) and
[NOTICE](NOTICE). Licenses for bundled third-party components are collected in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
