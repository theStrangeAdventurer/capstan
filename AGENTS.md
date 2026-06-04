# AGENTS.md

This project is cli llm agent like opencode and claude code

## Build

```
make clean && make
```

All `src/*.c` compiled as single gcc invocation, no per-file objects.
`compile_commands.json` is stale — ignore it, use the Makefile.

## Architecture — non-obvious

### Init order (critical)

`plugins_init()` in `src/plugins.c`:
1. `luaL_newstate` → `luaL_openlibs`
2. `http_init(L)` — registers global `http = {get, post, post_stream}`
3. `agent_init(L)` — registers global `agent = {append}`
4. `luaL_dofile(L, "ai/providers.lua")` — side-effect: sets `_G.on_messages`

Plugins are loaded AFTER this — they can use `http` and `agent` globals.

### Message flow (the tricky part)

After ANY Enter (command or plain text):
```
add_message(text, MSG_USER)       // user/plugin text
add_message("", MSG_AGENT)        // empty green placeholder FIRST
agent_emit(L)                     // THEN emit — builds history, calls _G.on_messages
```

Why this order: `agent_emit` filters empty messages (`text[0] == '\0'`) so the empty AGENT doesn't go to the LLM. But the AGENT message exists in the array — so `agent.append(chunk, "agent")` from Lua callbacks finds and fills it.

### Memory ownership

- `add_message(text, raw_text, role)` — **takes ownership** of both pointers. Caller must pass malloc'd strings (e.g., `my_strdup`). No strdup inside.
- `append_to_last_message` — `realloc`s `m->raw_text`, sets `m->text = m->raw_text`.
- `clear_messages` — frees both `text` and `raw_text` (checks `raw_text != text` to avoid double-free).

### Streaming

`http.post_stream(url, body, headers, callback)` — async: registers with `curl_multi`, returns `async_id` immediately. Callback receives `(raw_bytes, is_done)`.

`http_poll(L)` must be called from main loop to drive curl_multi. It's called in the `ch == ERR` (idle) branch.

### SSE / providers

All SSE parsing is in Lua (`ai/providers.lua`). C only passes raw bytes. Provider-specific `on_chunk` overrides the OpenAI-compatible `default_on_chunk`.

### Plugin model

- Plugin handlers receive a **ctx table** (not raw `input` string). Signature: `handler(ctx)`.
- Handler returns `(ui_result, llm_result)` or calls `ctx:replace(ui_val, llm_val?)`.
- Plugins live in `plugins/*.lua`, returned table has `.id`, `.command`, `.handler`.

**ctx table fields:**

| Field | Type | Description |
|-------|------|-------------|
| `ctx.input` | string | Full original user input (unchanged) |
| `ctx.command` | string | Matched command, e.g. `"/ip"` |
| `ctx.args` | table | Array of space-split arguments after the command |
| `ctx:replace(ui_val, llm_val?)` | function | Returns `(ui_val, llm_val)` — wraps the two return values for clarity. If `llm_val` is omitted, it defaults to `ui_val`. |

**Command parsing rules:**
- Commands must be the **first non-whitespace token** in the input.
- Everything after the command token is split by spaces into `ctx.args` (1-indexed Lua array).
- Example: `  /hi Fox v2` → `ctx.command = "/hi"`, `ctx.args = {"Fox", "v2"}`.
- If input has no leading `/` at the start, it is treated as plain text (no plugin invoked).

**Blocking HTTP in plugins:**
- `http.get()` / `http.post()` use `curl_multi` internally and call `render_all()` in a spin loop — UI shows a braille spinner `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏` during the request.
- `http.post_stream()` is fully async, data arrives via callbacks, no spinner needed.

### Apperance
- Spinner at `(rows-1, MARGIN+1)` shows during any active curl transfer.
- Cursor hidden while loading, restored after.

## Conventions

- C99, `-Wall -Wextra -Werror`, `-D_POSIX_C_SOURCE=200112L`
- `l_` prefix for Lua-bound C functions
- `my_strdup()` instead of POSIX `strdup()`
- ncurses + Lua linked statically (`.a`), libcurl dynamic (`-lcurl`)
- Dynamic arrays: double capacity when full (not fixed increment)
