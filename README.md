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
[Markdown report](BENCHMARK_REPORT.md).

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
passed to vision-capable OpenAI-compatible models. Formats are checked by file
signature, decoded images are limited to 10 MiB each, and image base64 is not
written to runtime logs.

The `subagents` model tool can run focused internal tasks concurrently. Child
agents can receive narrower model, turn, concurrency, and tool limits. Network
waits are parallel; Lua callbacks and tool handlers remain single-threaded.

## Permissions And Safety

Permissioned model-initiated tools go through Capstan's permission policy.
Rules may allow, deny, or ask, and the TUI can approve one call, one tool for the
current run, all tools for the current run, or persist an exact allow rule.

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

Persisted `Always allow` rules are stored in:

```text
$XDG_STATE_HOME/capstan/permissions.lua
```

or, when `XDG_STATE_HOME` is unset:

```text
~/.local/state/capstan/permissions.lua
```

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
- `--full-control` for non-persisted workspace-scoped permission grants
- `--benchmark` for isolated eval mode
- `--json` for `{ok, text, error}` output

`--benchmark` implies `--no-mcp --no-wiki --full-control` and excludes local
prompt overrides, external skills, `AGENTS.md`, global plugins, and hooks so
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
