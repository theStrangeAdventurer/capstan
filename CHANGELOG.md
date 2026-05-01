# CHANGELOG.md

## Session 1: Lua Plugin System Foundation

### Added

#### Core Plugin Infrastructure
- **include/plugins.h**
  - `Plugin` struct with metadata (id, name, description, command)
  - Plugin state management (Lua state, handler reference)
  - Async operation support fields (async_id, is_processing, callback)
  - `PluginCallback` typedef for plugin result callbacks
  - `PluginRegistry` struct for managing loaded plugins

- **src/plugins.c**
  - Global Lua VM initialization (`plugins_init()`)
  - Plugin loading from .lua files (`plugin_load()`)
  - Plugin registry system with dynamic array management
  - Plugin lookup by command (`plugin_registry_find()`)
  - Synchronous plugin execution (`plugin_execute_sync()`)
  - Memory cleanup on shutdown (`plugin_registry_cleanup()`)
  - Helper function for safe Lua string field extraction

#### Plugin System Features
- Lua integration with ncurses (static linking)
- Safe parameter passing to plugins
- Plugin metadata extraction from Lua tables
- Handler function reference storage via Lua registry
- Optional dual-output support (UI display + LLM context)

#### Plugins
- **plugins/hi.lua** - Greeting plugin returning emoji response
- **plugins/file.lua** - File reading plugin (skeleton)

### Fixed

#### Build Issues
- Added `-lm` flag for Lua math library linking
- Fixed undefined references to `pow()`, `sin()`, `cos()`, etc.

#### Encoding & Terminal Issues
- UTF-8 multi-byte character handling in ncurses
- Backspace deletion for multi-byte UTF-8 sequences
- Proper terminal cleanup on program exit with `endwin()`
- Fixed cursor lag when editing long strings with emoji/Cyrillic

#### Code Quality
- Optimized `redraw()` to use `addstr()` instead of `mvprintw()` for UTF-8
- Proper memory management in plugin registry
- Launch protection in plugin functions
- Include guards and forward declarations in headers

### Changed

#### main.c
- Integrated plugin system initialization
- Plugin directory scanning and loading
- Command detection and plugin routing
- Result handling with proper buffer management
- Added helper function `replace_input()` for result display

#### Code Style
- Consistent naming conventions (snake_case for functions)
- Comprehensive comments in Russian/English
- Separated concerns (plugin management vs. UI)

### Technical Details

#### Architecture
- Single global Lua VM shared across all plugins
- Dynamic plugin registry with O(n) lookup (acceptable for 5-10 plugins)
- Synchronous-only execution (async planned for Week 2)
- Plugin results support optional dual output:
  - First return value: UI display (user sees this)
  - Second return value: LLM context (optional, defaults to first value)

#### Memory Management
- Safe string copying with bounds checking
- Proper Lua state cleanup with `lua_close()`
- Plugin structure cleanup with `luaL_unref()` for handler references
- Dynamic registry reallocation on demand

#### Known Limitations
- No async I/O yet (planned for `/file` and `/fetch` plugins)
- No plugin sandboxing (Lua can access full stdlib)
- Single Lua state per VM (not per-plugin isolation)
- File size limits not enforced yet

### Testing
- Successfully loaded and executed `/hi` plugin
- UTF-8 input/output working correctly
- Terminal properly restores on exit
- Plugin result substitution functional

### Next Steps 

- Реализовать лэйаут для ncurses - чтобы помимо input'a в который пишет пользователь - был еще блок нефиксированой высоты в котором хранится вся история сообщений от пользователя и от llm
- Implement HTTP fetch on both sides (C and lua) - подробности спроси у пользователя
- Implement async task manager (`src/async.c`)

