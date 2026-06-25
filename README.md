# Capstan

Capstan is a small terminal coding agent built in C with an embedded Lua agent
runtime. It is designed to be local, inspectable, and easy to extend with plain
Lua files.

## Why It Exists

Capstan is not trying to be the largest coding-agent platform. The project is
optimized for a different shape:

- a tiny native TUI binary with vendored ncurses and Lua;
- OpenAI-compatible providers configured from one Lua config file;
- hot-reloadable Lua plugins under `~/.config/capstan/plugins/`;
- explicit permissions for model-initiated tools;
- opt-in self-improvement through durable user plugins and hooks.

## Build

```sh
./build.sh
```

The script checks system dependencies, builds vendored ncurses and Lua, embeds
the Lua runtime/plugins into the binary, and writes:

```text
build/capstan
```

Current local macOS builds are roughly in the hundreds of kilobytes; measure
your build with:

```sh
ls -lh build/capstan
```

## System Dependencies

macOS:

```sh
xcode-select --install
```

Linux, Debian/Ubuntu:

```sh
sudo apt install build-essential libcurl4-openssl-dev
```

Linux, Fedora:

```sh
sudo dnf install gcc make libcurl-devel
```

`libcurl` is the only dynamic runtime dependency. ncurses and Lua are linked
from vendored static archives.

## Run

```sh
OPENAI_API_KEY=... ./build/capstan
```

or:

```sh
DEEPSEEK_API_KEY=... ./build/capstan
```

Smoke-test the embedded runtime without opening the TUI:

```sh
./build/capstan --self-test-embedded
```

Headless one-shot mode:

```sh
./build/capstan run --prompt "Inspect this repository"
./build/capstan run --prompt-file task.md --json
```

## Config

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
  permissions = {
    { tool = "file_read", pattern = "~/code/project/*", allow = true },
    { tool = "fetch", pattern = "https://api.github.com/*", allow = true },
  },
  capabilities = {
    self_improvement = false,
  },
}
```

Provider environment variables still override config where explicitly supported
by the runtime. Runtime choices such as `/models` selections are stored in the
state directory, not rewritten into `config.lua`.

## Built-In Commands

- `/file` reads files and opens the file finder with Tab.
- `/write` writes files.
- `/edit` replaces exact file fragments.
- `/shell` runs shell commands.
- `/fetch` fetches HTTP/HTTPS URLs.
- `/logs` shows recent runtime logs.
- `/skills` lists loaded skills.
- `/models` selects a model for the current provider from the provider API.

## CLI Run Mode

`capstan run` executes one agent task without opening the TUI. It accepts
`--prompt`, `--prompt-file`, stdin, `--provider`, `--model`, `--workdir`,
`--max-turns`, and `--json`.

## Permissions

Permissions protect tool calls initiated by the model. Manual slash commands are
direct user intent and do not go through the model-tool permission prompt.

If the model wants to call a tool, Capstan checks `permissions` rules first. If
no rule matches, it may ask in the TUI. `Always allow` choices are persisted in:

```text
$XDG_STATE_HOME/capstan/permissions.lua
```

or:

```text
~/.local/state/capstan/permissions.lua
```

## Plugins And Hooks

User plugins are separate Lua files:

```text
~/.config/capstan/plugins/*.lua
```

Capstan watches this directory while running. New, changed, and deleted plugin
files are hot-reloaded without restarting the process. Plugins can provide slash
commands, autocomplete, model tools, and agent hooks.

Minimal plugin:

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

The built-in `self-improvement` skill is disabled by default. It becomes
available only when this explicit high-risk capability is enabled:

```lua
return {
  capabilities = {
    self_improvement = true,
  },
}
```

Subagents are enabled by default. Set `capabilities.subagents = false` to hide
the `subagents` model tool. The tool lets the orchestrator run several focused
internal agents in parallel. It has no separate permission pattern; use
`subagents.max_*` config limits for scale, while the tools used inside each
subagent still pass through their normal permissions.

## Terminal Compatibility

Capstan builds ncurses with built-in fallback descriptions for common terminal
types: `xterm-256color`, `tmux-256color`, `screen-256color`, `xterm`, `screen`,
`ansi`, and `vt100`.

You should not need to export `TERMINFO` manually for ordinary terminal, tmux,
or screen setups. If startup fails, Capstan prints the current `TERM` and a
diagnostic before opening the TUI.

## Known Limitations

- Subagents run in-process; network waits are parallel, Lua callbacks/tool
  handlers remain single-threaded.
- User Lua plugins are trusted local code, not sandboxed isolates.
- Blocking Lua plugin work can still block UI cancellation until it returns.
- `libcurl` is provided by the operating system.

## Project Layout

```text
src/        C runtime, TUI, permissions, plugin loader, HTTP bridge
include/    C headers
agent/      embedded Lua agent runtime
plugins/    embedded built-in Lua plugins
skills/     gated built-in skills
specs/      behavior and architecture specs
vendor/     vendored ncurses, Lua, munit, and Lua JSON
```

## Development Checks

```sh
make test
make test-http-lua
make test-build
```
