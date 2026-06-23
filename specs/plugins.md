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

The built-in `self-improvement` skill, when explicitly enabled, instructs the
agent to write durable extensions here instead of embedding plugin code inside
`config.lua`.

## Hot Reload

Capstan watches the user plugin directory while the process is running. The
watcher uses a portable polling loop from the main idle branch, so no platform
specific `inotify` or `kqueue` dependency is required.

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

## Tests

The embedded smoke test verifies built-in plugin availability. Runtime reload is
covered by the application build path; future C integration tests should cover
watcher edge cases if the plugin loader is split away from Lua/TUI dependencies.
