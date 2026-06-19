# AGENTS.md

This project is cli llm agent like opencode and claude code

## Build

### Quick start

```sh
./build.sh
```

Cross-platform: macOS (arm64 / x86_64) and Linux. The script checks system
dependencies, builds vendored ncurses + Lua, then compiles the project.

For a project-only rebuild (ncurses + Lua already built):

```sh
make clean && make
```

`compile_commands.json` is stale — ignore it, use the Makefile.

### Build principles

**Static linking** — ncurses and Lua are linked as `.a` archives.
- Zero runtime dependencies besides `libcurl` (used for LLM API calls).
- Binary is a single Mach-O / ELF file — portable, no shared libs needed.
- ncurses: `--enable-widec` (wide-char for Unicode), `--with-termlib`
  (separates terminal constants like `_COLS` into `libtinfow.a`).
  `--without-shared` — only static `.a` needed, skip `.so`/`.dylib`.

**Single compilation unit** — all `src/*.c` compiled in one `gcc` invocation.
- No per-file `.o` objects, no incremental build.
- Simpler Makefile, faster link step.
- Trade-off: full recompile on any change (mitigated by small codebase ~1500 lines).

**Vendored, not system packages** — ncurses and Lua built from source in `vendor/`.
- Avoids version mismatches (e.g. Lua 5.4 vs 5.5, ncurses API changes).
- `libtinfow.a` must come from the same ncurses build — system ncurses may
  not provide it (`--with-termlib` is non-default).
- Build is idempotent — `build.sh` can be re-run after OS switch or arch change.

**Cross-platform** — `build.sh` auto-detects OS at runtime.
- macOS: `sysctl -n hw.ncpu`, Xcode CLT, libcurl via SDK `.tbd` stubs.
- Linux: `nproc`, `build-essential`/`libcurl-dev` via `apt`/`dnf`.
- ncurses and Lua auto-detect the platform in their own `configure`/`Makefile`.

### Makefile breakdown

```
CC      = gcc                     # Apple Clang on macOS, GCC on Linux
CFLAGS  = -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L
          + -Iinclude -Ivendor/... (headers)

LDFLAGS = vendor/lua-5.5.0/src/liblua.a
        + vendor/ncurses-install/lib/libncursesw.a
        + vendor/ncurses-install/lib/libtinfow.a
        + -lm -lcurl
          \____________________ ____/  \__/  \___/
                               |        |      |
                     vendored static   math  system
```

- Static `.a` files are passed as **direct paths** (not `-L`/`-l`) to avoid
  accidentally picking up wrong system libraries.
- `-D_POSIX_C_SOURCE=200112L` — enables POSIX.1-2001 (`fork`, `pipe`,
  `sigaction`, etc.).
- `ncursesw` — the wide-char variant (`wchar_t`, Unicode).
- `tinfow` — terminfo library separated by `--with-termlib`; contains
  `_COLS`, `cur_term`, and other terminal capability constants.
- `-lcurl` — only dynamic dependency (linked from system).
- `-lm` — math library, pulled in by Lua.

### Directory layout after build

```
vendor/ncurses-install/    # ncurses headers + static .a libs (gitignored)
vendor/lua-5.5.0/src/*.o  # Lua object files (gitignored)
build/capstan              # final binary (gitignored)
```

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

## Testing

### Setup

µnit is vendored at `vendor/munit/` (`munit.h` + `munit.c`, copied directly
from [upstream](https://github.com/nemequ/munit)). No git submodule, no
installation — files are part of the repo.

### Run

```sh
make test
```

Test binary does **not** link ncurses, Lua, or curl — only the pure C modules
that have no UI or Lua dependencies.

### Build smoke test

```sh
make test-build
```

This builds `build/capstan`, copies only that binary into a fresh
`/tmp/capstan-build-smoke.*` directory, and runs the copied binary with
`--self-test-embedded` from an isolated working directory. The check verifies
that the embedded system prompt and built-in plugin commands are available
without `ai/` or `plugins/` beside the binary.

The smoke directory is intentionally left on disk. The script prints a command
like this so you can launch the copied binary manually and verify the interactive
plugins:

```sh
cd /tmp/capstan-build-smoke.XXXXXX/run && HOME=/tmp/capstan-build-smoke.XXXXXX/home /tmp/capstan-build-smoke.XXXXXX/bin/capstan
```

### Currently tested modules

| Module | Tests | File |
|--------|-------|------|
| `input.c` | 12 tests (ASCII insert, UTF-8 nav, backspace, clear) | `test/test_input.c` |
| `scroll.c` | 9 tests (up/down, clamp, reset, set) | `test/test_scroll.c` |
| `utils.c` | 5 tests (my_strdup, replace_with) | `test/test_utils.c` |

### Adding new tests

1. Create `test/test_<module>.c` with a `MunitTest` array and exported `MunitSuite`.
2. Add the `.c` file to `TEST_SRCS` in the Makefile.
3. Register the suite in `test/test_main.c`.

Example test file:

```c
#include "munit.h"
#include "my_module.h"

static MunitResult test_something(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  munit_assert_int(my_func(), ==, 42);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/something", test_something, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite my_module_suite = {"/my_module", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
```

### Testable vs untestable modules

Modules that depend on ncurses, Lua, or curl cannot be unit-tested without
those libraries. The current split:

| Testable (no deps) | Untestable (needs ncurses/Lua/curl) |
|---------------------|--------------------------------------|
| `input.c` | `tui.c` (ncurses) |
| `scroll.c` | `agent.c` (Lua) |
| `utils.c` | `plugins.c` (Lua) |
| `dispatch.c` (logic only) | `http.c` (curl) |
| `permit.c` (`permit_pattern_match`) | `popup.c` (ncurses) |

To make more modules testable, extract pure-logic functions that don't call
ncurses/Lua/curl APIs, and put them in separate source files.

### Test policy

- **New feature → new tests.** If the added logic can be tested without
  ncurses/Lua/curl, it must have unit tests. If it requires dependencies —
  extract the pure-logic part into a separate function/module so it becomes
  testable.
- **Changed code → update tests.** If you modify a function that has tests,
  run `make test` and fix any breakage. If the change adds new behavior,
  add a new test case for it.

## Specs

- **New feature → new spec.** Add or update a focused markdown spec in `specs/`
  for every user-visible feature. The spec should cover behavior, architecture
  decisions, constraints, and test notes.
- Link feature specs from this section so future agents can discover them.

Feature specs:

- [Embedded runtime assets](specs/embedded-runtime-assets.md)
- [Focus modes](specs/focus-modes.md)
- [Editor command](specs/editor-command.md)
- [Popups](specs/popups.md)
- [Fetch plugin](specs/fetch-plugin.md)

## Conventions

- C99, `-Wall -Wextra -Werror`, `-D_POSIX_C_SOURCE=200112L`
- `l_` prefix for Lua-bound C functions
- `my_strdup()` instead of POSIX `strdup()`
- ncurses + Lua linked statically (`.a`), libcurl dynamic (`-lcurl`)
- Dynamic arrays: double capacity when full (not fixed increment)
