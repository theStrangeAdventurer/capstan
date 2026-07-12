# AGENTS.md

Capstan is a CLI LLM agent, similar to OpenCode and Claude Code.

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
- No runtime dependencies besides `libcurl` (used for LLM API calls).
- The binary is a single Mach-O / ELF file — portable, no shared libs needed.
- ncurses: `--enable-widec` (wide-char for Unicode), `--with-termlib`
  (separates terminal constants like `_COLS` into `libtinfow.a`).
  `--without-shared` — only static `.a` archives are needed; `.so`/`.dylib` are skipped.

**Single compilation unit** — all `src/*.c` compiled in one `gcc` invocation.
- No per-file `.o` objects, no incremental build.
- Simpler Makefile, faster link step.
- Trade-off: full recompile on any change (acceptable for a project this size).

**Vendored, not system packages** — ncurses and Lua built from source in `vendor/`.
- Avoids version mismatches (e.g. Lua 5.4 vs 5.5, ncurses API changes).
- `libtinfow.a` must come from the same ncurses build — system ncurses may
  not provide it (`--with-termlib` is non-default).
- Build is reproducible — `build.sh` can be re-run after OS switch or arch change.

**Cross-platform** — `build.sh` auto-detects OS at runtime.
- macOS: `sysctl -n hw.ncpu`, Xcode CLT, libcurl via SDK `.tbd` stubs.
- Linux: `nproc`, `build-essential`/`libcurl-dev` plus `infocmp`
  (`ncurses-bin` on Debian/Ubuntu) via `apt`/`dnf`.
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
          \______________________/  \__/  \___/
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

### Architecture policy

When adding or changing behavior that affects multiple code paths:

- Prefer one canonical implementation of the policy or business rule over
  duplicated partial implementations.
- If the behavior must exist in multiple layers, define which layer owns the
  policy and which layers are adapters, caches, fallbacks, or compatibility
  shims.
- Prefer integrating with the existing configuration model instead of adding
  hidden constants or parallel configuration paths.
- Before implementing, identify all affected data paths: UI, model context,
  logs, persisted state, command-line mode, tests, and plugin/runtime APIs.
- Define the failure mode explicitly: fail open, fail closed, preserve old
  behavior, or surface an error.
- Add tests for the intended behavior and for important false positives or
  regressions.

Avoid fixing only the observed symptom when the same rule clearly applies
across multiple paths. First find the owner of the rule, then route callers
through that owner.

### Init order (critical)

`plugins_init()` in `src/plugins.c`:
1. `luaL_newstate` → `luaL_openlibs`
2. `package.path` is configured with `~/.config/capstan/?.lua`
3. Embedded Lua modules are registered in `package.preload`
4. `http_init(L)` — registers global `http = {get, post, post_stream, ...}`
5. `agent_init(L)` — registers global `agent = {append, set_info, ...}`
6. `tools_init(L)` — registers built-in tool helpers
7. `mcp_init(L)` — registers global `mcp = {spawn, send, recv, alive, kill}`
8. `capstan` runtime paths are registered
9. User config and persisted runtime state are loaded into `capstan`
10. `permit`, `log`, and `popup` globals are registered
11. The embedded system prompt plus project instructions/skills are loaded
12. `agent/runtime.lua` is loaded — side-effect: sets `_G.agent_entry`

Plugins are loaded AFTER this — they can use the registered globals.

### Message flow (the tricky part)

After a non-empty Enter (command or plain text, or empty Enter with buffered results pending):
```
add_message(ui_text, raw_text, MSG_USER)  // user/plugin text
add_message(empty, empty, MSG_AGENT)      // empty green placeholder FIRST
agent_build_and_dispatch(L)              // THEN build & dispatch — builds history, calls _G.agent_entry
```

Why this order: `agent_build_and_dispatch` filters empty messages (`text[0] == '\0'`) so the empty AGENT doesn't go to the LLM. But the AGENT message exists in the array — so `agent.append(chunk, "agent")` from Lua callbacks finds and fills it.

### Memory ownership

- `add_message(text, raw_text, role)` — **takes ownership** of both pointers. Caller must pass malloc'd strings (e.g., `my_strdup`). The function does not call strdup internally.
- `append_to_last_message` — `realloc`s `m->raw_text`, sets `m->text = m->raw_text`.
- `clear_messages` — frees both `text` and `raw_text` (checks `raw_text != text` to avoid double-free).

### Streaming

`http.post_stream(url, body, headers, callback)` — async: registers with `curl_multi`, returns `async_id` immediately. Callback receives `(raw_bytes, is_done)`.

`http_poll(L)` must be called from main loop to drive curl_multi. It's called in the `ch == ERR` (idle) branch.

### SSE / providers

All SSE parsing is in Lua (`agent/stream.lua`). C only passes raw bytes. Provider-specific `on_chunk` overrides the OpenAI-compatible `default_on_chunk`.

### Plugin model

- Plugin handlers receive a **ctx table** (not raw `input` string). Signature: `handler(ctx)`.
- Handler returns `(ui_result, llm_result)` or calls `ctx:replace(ui_val, llm_val?)`.
- Plugins live in `plugins/*.lua`, returned table has `.id`, `.command`, `.handler`.

**ctx table fields:**

| Field | Type | Description |
|-------|------|-------------|
| `ctx.input` | string | Full original user input (unchanged) |
| `ctx.command` | string | Matched command, e.g. `"/file"` |
| `ctx.args` | table | Array of space-split arguments after the command |
| `ctx:replace(ui_val, llm_val?)` | function | Returns `(ui_val, llm_val)` — wraps the two return values for clarity. If `llm_val` is omitted, it defaults to `ui_val`. |

**Command parsing rules:**
- Commands must be the **first non-whitespace token** in the input.
- Everything after the command token is split by spaces into `ctx.args` (1-indexed Lua array).
- Example: `  /file README.md` → `ctx.command = "/file"`, `ctx.args = {"README.md"}`.
- If input has no leading `/` at the start, it is treated as plain text (no plugin invoked).

**Blocking HTTP in plugins:**
- `http.get()` / `http.post()` use `curl_multi` internally and call `render_all()` in a spin loop — UI shows a pulsing dot loader (`·` → `•` → `●`, 8 frames) at `(rows-1, MARGIN+1)`, with an italic label `"Thinking"` (red) or `"Answering"` (dim) next to it, while the request is active.
- `http.post_stream()` is fully async, data arrives via callbacks, no spinner needed.

### Appearance
- Spinner at `(rows-1, MARGIN+1)` shows during any active curl transfer.
- The cursor is hidden while loading and restored afterwards.

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
without `agent/`, `ai/`, or `plugins/` beside the binary.

The smoke directory is intentionally left on disk. The script prints a command
like this so you can launch the copied binary manually and verify the interactive
plugins:

```sh
cd /tmp/capstan-build-smoke.XXXXXX/run && HOME=/tmp/capstan-build-smoke.XXXXXX/home /tmp/capstan-build-smoke.XXXXXX/bin/capstan
```

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
those libraries. To make more modules testable, extract pure-logic functions
that don't call ncurses/Lua/curl APIs, and put them in separate source files.

See `TEST_SRCS` in the Makefile for the current list of testable modules.

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

- [Agent control](specs/agent-control.md)
- [Agent profiles](specs/agent-profiles.md)
- [Agent loop](specs/agent-loop.md)
- [CLI run mode](specs/cli-run.md)
- [CI binaries](specs/ci-binaries.md)
- [Compact command](specs/compact-command.md)
- [Config](specs/config.md)
- [Embedded runtime assets](specs/embedded-runtime-assets.md)
- [Focus modes](specs/focus-modes.md)
- [Hooks](specs/hooks.md)
- [Info command](specs/info-command.md)
- [Input history](specs/input-history.md)
- [Editor command](specs/editor-command.md)
- [Popups](specs/popups.md)
- [Plugins](specs/plugins.md)
- [Project instructions](specs/project-instructions.md)
- [Public readiness](specs/public-readiness.md)
- [Fetch plugin](specs/fetch-plugin.md)
- [File read plugin](specs/file-read-plugin.md)
- [File edit plugin](specs/file-edit-plugin.md)
- [File write plugin](specs/file-write-plugin.md)
- [Models command](specs/models-command.md)
- [OAuth auth](specs/oauth-auth.md)
- [Permissions](specs/permissions.md)
- [Runtime logs](specs/runtime-logs.md)
- [Runtime state](specs/runtime-state.md)
- [Shell plugin](specs/shell-plugin.md)
- [Skills](specs/skills.md)
- [Self improvement](specs/self-improvement.md)
- [Start screen](specs/start-screen.md)
- [Subagents](specs/subagents.md)
- [Terminal runtime](specs/terminal-runtime.md)
- [Workspace directory](specs/workspace-directory.md)
- [Wiki](specs/wiki.md)
  - Built-in `wiki-onboarding` skill guides first-time setup when `/wiki` is run without configured `wiki.path`.
- [MCP client](specs/mcp-client.md)

## Conventions

- `gnu99`, `-Wall -Wextra -Werror`, `-D_POSIX_C_SOURCE=200112L`
- `l_` prefix for Lua-bound C functions
- `my_strdup()` instead of POSIX `strdup()`
- ncurses + Lua linked statically (`.a`), libcurl dynamic (`-lcurl`)
- Dynamic arrays: double capacity when full (rather than a fixed increment)
