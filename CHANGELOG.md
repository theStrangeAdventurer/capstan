# CHANGELOG.md

## Session 2: HTTP + Agent + Streaming → LLM

### HTTP Streaming (`src/http.c`)

- **`curl_multi`** — добавлен глобальный `CURLM *multi_handle` для неблокирующих запросов
- **`StreamCtx`** — контекст стрима: `CURL *easy`, `lua_State *L`, колбек-реф, хедеры
- **Динамический массив `streams[]`** — с удвоением при заполнении (амортизированная O(1) вставка)
- **`stream_write_cb`** — колбек curl: получает сырые TCP-чанки и сразу вызывает Lua `callback(raw, false)`
- **`l_http_post_stream`** — `http.post_stream(url, body, headers, callback)` → возвращает `async_id`, запрос регистрируется в `curl_multi`
- **`http_poll(L)`** — дёргает `curl_multi_perform`, при `CURLMSG_DONE` вызывает `callback(nil, true)`, чистит хендлы и компактит массив стримов
- **`http_init`** — инициализирует `multi_handle`, регистрирует `http.post_stream`

### Agent Lua API (`src/agent.c`)

- **`l_agent_append`** — `agent.append(text, role)` с дефолтной ролью `MSG_USER`
- **`agent_emit`** — строит Lua-таблицу `{ {role, content}, ... }` из Messages, пропускает пустые сообщения, вызывает `_G.on_messages(history)`
- **`append_to_last_message`** — авто-создаёт сообщение если нет последнего нужной роли
- **`agent_init`** — регистрирует глобальную таблицу `agent` с полем `append`

### Main loop (`src/main.c`)

- **`http_poll(L)`** в idle-ветке — драйвит curl_multi на каждом цикле
- **`else` для неизвестных команд** — `Unknown command: /xxx`
- **`add_message("", MSG_AGENT)` + `agent_emit(L)`** — после любого ввода добавляется пустой зелёный плейсхолдер, затем C собирает историю и вызывает `on_messages`

### TUI (`src/tui.c`)

- **`count_message_lines()`** — подсчёт строк с учётом `\n` и ширины окна
- **Рендеринг по визуальным строкам** — сканирует до `\n` или `inner_w` вместо тупого сдвига на `inner_w`

### Plugins system (`src/plugins.c`)

- **`+ agent_init(L)`** — регистрирует `agent` global
- **`+ luaL_dofile(L, "ai/providers.lua")`** — загружает провайдеры и сеттит `_G.on_messages`

### Headers

- **`include/plugins.h`** — `+ extern lua_State *L`
- **`include/agent.h`** — `+ void agent_init(L)`, `+ void agent_emit(L)`

### Lua

| Файл | Что |
|---|---|
| `ai/providers.lua` | `M.default_on_chunk` (OpenAI-совместимый SSE-парсинг), `M.stream()` (буферизация `\n\n` + кастомный `on_chunk` на провайдера), `_G.on_messages` (отправка в LLM через `http.post_stream` + `agent.append`) |
| `plugins/stream_test.lua` | `/stream` — тест `http.post_stream` → `agent.append` на httpbin |
| `plugins/http_post_example.lua` | `/post` — синхронный `http.post` JSON |

---

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

