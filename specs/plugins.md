# Plugins

Plugins are Lua files that extend Capstan with slash commands, autocomplete,
model tools, and hooks.

## Paths

Built-in plugins are embedded into the binary. User plugins live in:

```text
~/.config/capstan/plugins/*.lua
```

Each plugin file returns a Lua table. `id` is required. `command` and `handler`
are required only for slash commands; hook-only plugins may omit them.

Model-tool handlers return successful UI/model values with
`ctx:replace(ui_value, model_value?)`. Expected operational failures should use
`ctx:error(ui_value, model_value?)`; the runtime then preserves the supplied
message while marking the tool row and runtime event as an error. Throwing is
reserved for unexpected plugin defects and adds a traceback diagnostic.

The built-in `self-improvement` skill, when explicitly enabled, instructs the
agent to write durable extensions here instead of embedding plugin code inside
`config.lua`.

Plugins are trusted local code, not a security sandbox. A plugin can use Lua
standard libraries and Capstan runtime APIs with the same local authority as the
Capstan process. Only install or generate plugins you are willing to run as
local code. The self-improvement skill is disabled unless
`capabilities.self_improvement = true` is set explicitly.

## Hot Reload

Capstan watches the user plugin directory while the process is running. The
watcher uses a portable polling loop from the main idle branch, so no platform
specific `inotify` or `kqueue` dependency is required.

Headless `capstan run` loads user plugins from the same directory once at
startup. It does not watch for changes during the run.

- New `.lua` files are loaded automatically.
- Changed `.lua` files are reloaded automatically.
- Deleted `.lua` files are removed; built-in plugins are restored if a deleted
  user plugin was overriding one.
- If a changed plugin fails to load, the previous working version remains active
  and the watcher retries on later scans.
- Plugin hooks are removed and reinstalled with the plugin, so reloads do not
  duplicate old hook functions.

## History Control

Slash command plugins are added to pending LLM context by default. Commands that
only mutate runtime state should opt out:

```lua
plugin.history = false
```

No-history commands show UI feedback but do not add messages, do not add pending
context, and do not trigger an agent request. `/models` uses this mode.

## Lua Language Server

Capstan ships LuaLS annotations in `types/capstan.d.lua` and `.luarc.json`.
Annotate plugin tables to get completion for plugin fields, ctx helpers, and
runtime globals:

```lua
---@type CapstanPlugin
local plugin = {}
```

## Ecosystem

Capstan has three extension tiers. The table below clarifies what each tier is
for and how they relate.

| | Shell script | Plugin | Skill |
|---|---|---|---|
| **What** | Standalone script (`reddit-top.sh`) | Lua file at `~/.config/capstan/plugins/*.lua` returning `{id, command, handler}` | Directory with `SKILL.md` at `.agents/skills/name/` or `~/.agents/skills/name/` |
| **Caller** | User (manually in terminal) | User (`/command`) or agent (via tool call) | Agent only — via system prompt instruction |
| **Knows Capstan?** | No | Yes — `http.get/post`, `agent.append`, hooks, ui | No code — only markdown instructions |
| **Use case** | One-off pipeline, pipe to `jq`, cron job | Execute HTTP, mutate state, render UI feedback | Teach agent *when and how* to use a plugin |

### Flow

```
Skill           «if the user wants X, call /plugin Y»  (markdown, in prompt)
    │
    ▼
Plugin          /plugin executes http, filters, returns (ui, llm)  (Lua, hot-reloaded)
    │
    ▼
Agent           receives llm_result in context, reasons about it, answers user
```

A plugin without a skill is a dumb command — the agent won't know when to call
it. A skill without a plugin is dead instructions — the agent has no code to
execute. Production features ship as a **plugin + skill pair**.

### When a shell script is enough

If the task is purely for the human (e.g., a cron job that emails weekly Reddit
digests), a shell script outside Capstan is simpler. No Lua, no hot-reload
constraints, no need to teach the agent.

## Tests

The embedded smoke test verifies built-in plugin availability. Runtime reload is
covered by the application build path; future C integration tests should cover
watcher edge cases if the plugin loader is split away from Lua/TUI dependencies.
