# Embedded Runtime Assets

Built-in Lua runtime assets are compiled into the `capstan` binary so a user can
copy a single executable and get the default agent runtime, system prompt,
vendored Lua modules, plugins, and gated built-in skills without also copying
`agent/`, `ai/`, `vendor/`, `plugins/`, or `skills/`.

## Behavior

- `~/.config/capstan/config.lua` is loaded when present and exposed as
  `capstan.config`. Missing config is not an error.
- `agent/runtime.lua` is loaded from the embedded asset and requires the other
  embedded `agent/` runtime modules. Provider settings are configured through
  `~/.config/capstan/config.lua`.
- `system_prompt` is a Lua global loaded from `~/.config/capstan/system_prompt.txt`
  when present, otherwise from the embedded `ai/system_prompt.txt` asset.
- Agent runtime files listed in `AGENT_RUNTIME_ASSETS` and core built-in
  plugins listed in `CORE_PLUGIN_ASSETS` in the Makefile are loaded from
  embedded assets first.
- User plugins from `~/.config/capstan/plugins/*.lua` are loaded after embedded
  plugins and watched for hot reload. If a user plugin returns the same
  `plugin.id` as a built-in plugin, it replaces the built-in plugin.
- `skills/self-improvement/SKILL.md` is embedded. It is loaded directly from
  memory as `embedded:skills/self-improvement/SKILL.md` only when
  `capabilities.self_improvement = true` is set in
  `~/.config/capstan/config.lua`.
- Runtime startup does not require a `plugins/` directory beside the binary.

## Architecture

`tools/embed_assets.sh` generates `build/embedded_assets.c` from the Lua and text
assets during `make`. The generated file exposes `EmbeddedAsset` records with the
original repository paths, byte contents, and sizes.

Lua code is evaluated with `luaL_loadbuffer` so embedded chunks keep meaningful
chunk names in Lua errors. User Lua config still uses `luaL_dofile`.

## Constraints

- Asset files are text files and are embedded as escaped C string literals.
- Adding a new agent runtime module requires adding its Lua file to
  `AGENT_RUNTIME_ASSETS` and preloading it in `src/plugins.c` if it is required
  by module name from embedded execution.
- Adding a new built-in plugin requires adding its Lua file to
  `CORE_PLUGIN_ASSETS` and rebuilding the binary.
- Adding a new embedded built-in skill requires adding its files to
  `EMBEDDED_ASSETS` and deciding the config gate that exposes it.
- User plugin override identity is `plugin.id`, not filename.

## Test Notes

`make test` does not link Lua, ncurses, or curl, so this feature is verified by
the application build. Behavior that can be split into pure C helpers should get
unit tests before it grows more complex.
