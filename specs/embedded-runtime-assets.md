# Embedded Runtime Assets

Built-in Lua runtime assets are compiled into the `capstan` binary so a user can
copy a single executable and get the default provider code, system prompt,
vendored Lua modules, and plugins without also copying `ai/`, `vendor/`, or
`plugins/`.

## Behavior

- `ai/providers.lua` is loaded from `~/.config/capstan/providers.lua` when that
  file exists, otherwise from the embedded `ai/providers.lua` asset.
- `system_prompt` is a Lua global loaded from `~/.config/capstan/system_prompt.txt`
  when present, otherwise from the embedded `ai/system_prompt.txt` asset.
- Core built-in plugins listed in `CORE_PLUGIN_ASSETS` in the Makefile are
  loaded from embedded assets first.
- User plugins from `~/.config/capstan/plugins/*.lua` are loaded after embedded
  plugins. If a user plugin returns the same `plugin.id` as a built-in plugin,
  it replaces the built-in plugin.
- Runtime startup does not require a `plugins/` directory beside the binary.

## Architecture

`tools/embed_assets.sh` generates `build/embedded_assets.c` from the Lua and text
assets during `make`. The generated file exposes `EmbeddedAsset` records with the
original repository paths, byte contents, and sizes.

Lua code is evaluated with `luaL_loadbuffer` so embedded chunks keep meaningful
chunk names in Lua errors. File-based config overrides still use `luaL_dofile`.

## Constraints

- Asset files are text files and are embedded as escaped C string literals.
- Adding a new built-in plugin requires adding its Lua file to
  `CORE_PLUGIN_ASSETS` and rebuilding the binary.
- User plugin override identity is `plugin.id`, not filename.

## Test Notes

`make test` does not link Lua, ncurses, or curl, so this feature is verified by
the application build. Behavior that can be split into pure C helpers should get
unit tests before it grows more complex.
