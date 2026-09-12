#include "app_config.h"
#include "dyn_arr.h"
#include "log.h"
#include "permit.h"
#include "shell_process.h"
#include "process_manager.h"
#include "process_observe.h"
#include "session_manager.h"
#include "tui.h"
#include "utils.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

extern lua_State *L;

static PermEntries g_entries = {0};

static PermEntry *matching_entry(const char *tool, const char *target) {
  for (int i = (int)g_entries.size - 1; i >= 0; i--) {
    PermEntry *entry = g_entries.items[i];
    if (strcmp(entry->tool, tool) == 0 &&
        permit_pattern_match(entry->pattern, target))
      return entry;
  }
  return NULL;
}

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void log_shell_start(int timeout, const char *command) {
  const char *cmd = command ? command : "";
  size_t size = strlen(cmd) + 128;
  char *msg = malloc(size);
  if (!msg) {
    log_event("tool", "shell start [log allocation failed]");
    return;
  }
  snprintf(msg, size, "shell start timeout=%d command=%s", timeout, cmd);
  log_event("tool", msg);
  free(msg);
}

static void log_shell_done(int exit_code, int timed_out, long long duration_ms,
                           const char *command) {
  const char *cmd = command ? command : "";
  size_t size = strlen(cmd) + 192;
  char *msg = malloc(size);
  if (!msg) {
    log_event("tool", "shell done [log allocation failed]");
    return;
  }
  snprintf(msg, size,
           "shell done exit=%d timed_out=%d duration_ms=%lld command=%s",
           exit_code, timed_out, duration_ms, cmd);
  log_event("tool", msg);
  free(msg);
}

const char *permit_config_dir(void) {
  static char path[512];
  if (app_config_dir(path, sizeof(path)) != 0)
    return NULL;
  return path;
}

PermState permit_check(const char *tool, const char *target) {
  PermEntry *entry = matching_entry(tool, target);
  if (entry)
    return entry->allow ? PERM_ALLOW : PERM_DENY;

  if (strcmp(tool, "shell") == 0)
    return PERM_ASK;

  if (strcmp(tool, "file_read") == 0) {
    return permit_file_read_check(app_workdir(), target);
  }

  return PERM_ASK;
}

void permit_grant(const char *tool, const char *pattern, int allow) {
  for (size_t i = 0; i < g_entries.size; i++) {
    PermEntry *e = g_entries.items[i];
    if (strcmp(e->tool, tool) == 0 &&
        strcmp(e->pattern, pattern) == 0) {
      e->allow = allow;
      return;
    }
  }

  PermEntry *e = malloc(sizeof(PermEntry));
  e->tool = my_strdup(tool);
  e->pattern = my_strdup(pattern);
  e->allow = allow;
  da_append(&g_entries, e);
}

void permit_load(const char *path) {
  if (!L)
    return;

  if (luaL_dofile(L, path) != LUA_OK) {
    lua_pop(L, 1);
    return;
  }

  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  int len = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    const char *tool = NULL, *pattern = NULL;
    int allow = -1;

    lua_getfield(L, -1, "tool");
    tool = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "pattern");
    pattern = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "allow");
    if (!lua_isnil(L, -1))
      allow = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (tool && pattern && allow != -1)
      permit_grant(tool, pattern, allow);

    lua_pop(L, 1);
  }

  lua_pop(L, 1);
}

void permit_save(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return;

  fprintf(f, "return {\n");
  for (size_t i = 0; i < g_entries.size; i++) {
    PermEntry *e = g_entries.items[i];
    char tool[PERMIT_MAX_TARGET * 2];
    char pattern[PERMIT_MAX_TARGET * 2];
    if (!permit_lua_escape_string(e->tool, tool, sizeof(tool)) ||
        !permit_lua_escape_string(e->pattern, pattern, sizeof(pattern)))
      continue;
    fprintf(f, "  {tool = \"%s\", pattern = \"%s\", allow = %s},\n",
            tool, pattern, e->allow ? "true" : "false");
  }
  fprintf(f, "}\n");
  fclose(f);
}

static void permit_load_config_permissions(lua_State *L) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return;
  }

  lua_getfield(L, -1, "permissions");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 3);
    return;
  }

  int len = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    lua_getfield(L, -1, "tool");
    const char *tool = lua_tostring(L, -1);
    lua_getfield(L, -2, "pattern");
    const char *pattern = lua_tostring(L, -1);
    lua_getfield(L, -3, "allow");
    int has_allow = !lua_isnil(L, -1);
    int allow = lua_toboolean(L, -1);

    if (tool && pattern && has_allow)
      permit_grant(tool, pattern, allow);

    lua_pop(L, 4);
  }

  lua_pop(L, 3);
}

static int l_permit_check(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *target = luaL_checkstring(L, 2);
  PermEntry *entry = matching_entry(tool, target);
  PermState s = permit_check(tool, target);
  switch (s) {
  case PERM_ALLOW:
    lua_pushstring(L, "allow");
    break;
  case PERM_DENY:
    lua_pushstring(L, "deny");
    break;
  default:
    lua_pushstring(L, "ask");
    break;
  }
  /* A second value preserves the distinction between an explicit owner rule
     and the permissive workspace-read default. Callers using one return value
     remain compatible. */
  lua_pushboolean(L, entry && entry->allow);
  return 2;
}

static int l_permit_grant(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *pattern = luaL_checkstring(L, 2);
  int allow = lua_toboolean(L, 3);
  permit_grant(tool, pattern, allow);
  return 0;
}

static int l_permit_save(lua_State *L) {
  char path[512];
  if (app_state_ensure_dir() != 0 ||
      app_state_path(path, sizeof(path), "permissions.lua") != 0) {
    lua_pushboolean(L, 0);
    return 1;
  }
  permit_save(path);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_permit_load(lua_State *L) {
  char path[512];
  if (app_state_path(path, sizeof(path), "permissions.lua") != 0) {
    lua_pushboolean(L, 0);
    return 1;
  }
  permit_load(path);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_permit_prompt(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *target = luaL_checkstring(L, 2);
  const char *result = tui_permit_prompt(tool, target);
  lua_pushstring(L, result);
  return 1;
}

void permit_init(lua_State *L) {
  char path[512];
  permit_load_config_permissions(L);
  if (app_state_path(path, sizeof(path), "permissions.lua") == 0)
    permit_load(path);

  lua_newtable(L);

  lua_pushcfunction(L, l_permit_check);
  lua_setfield(L, -2, "check");

  lua_pushcfunction(L, l_permit_grant);
  lua_setfield(L, -2, "grant");

  lua_pushcfunction(L, l_permit_save);
  lua_setfield(L, -2, "save");

  lua_pushcfunction(L, l_permit_load);
  lua_setfield(L, -2, "load");

  lua_pushcfunction(L, l_permit_prompt);
  lua_setfield(L, -2, "prompt");

  lua_setglobal(L, "permit");
}

/* NULL scope is reserved for manual UI calls; model calls always install an
 * explicit owner. Never accept ownership or PIDs from tool arguments. */
static char process_owner[128];
static int process_scoped;

static const char *tools_process_owner(void) {
  if (process_scoped) return process_owner;
  const char *owner = session_manager_active_id();
  return owner ? owner : "";
}

static int l_tools_process_scope(lua_State *L) {
  const char *owner = luaL_optstring(L, 1, NULL);
  if (owner && strlen(owner) >= sizeof(process_owner))
    return luaL_error(L, "process owner too long");
  if (process_scoped) lua_pushstring(L, process_owner);
  else lua_pushnil(L);
  snprintf(process_owner, sizeof(process_owner), "%s", owner ? owner : "");
  process_scoped = owner != NULL;
  process_manager_set_owner(tools_process_owner());
  return 1;
}

static int process_visible(const ProcessSnapshot *s) {
  /* Runtime-owned MCPs are shared, but never expose another session's child. */
  return !process_scoped || strcmp(s->owner, tools_process_owner()) == 0 ||
      (strcmp(s->owner, "runtime") == 0 && strcmp(s->kind, "mcp") == 0);
}

static void push_process(lua_State *L, const ProcessSnapshot *s) {
  lua_newtable(L);
#define PROCESS_STRING(field) lua_pushstring(L, s->field); lua_setfield(L, -2, #field)
#define PROCESS_NUMBER(field) lua_pushinteger(L, s->field); lua_setfield(L, -2, #field)
#define PROCESS_BOOL(field) lua_pushboolean(L, s->field); lua_setfield(L, -2, #field)
  PROCESS_STRING(id); PROCESS_STRING(kind); PROCESS_STRING(label);
  PROCESS_STRING(workdir); PROCESS_STRING(owner);
  PROCESS_NUMBER(pid); PROCESS_NUMBER(pgid); PROCESS_NUMBER(started_ms);
  PROCESS_NUMBER(finished_ms);
  PROCESS_BOOL(running); PROCESS_BOOL(stopping); PROCESS_BOOL(timed_out);
  PROCESS_BOOL(output_available); PROCESS_BOOL(truncated);
  if (!s->running) {
    lua_pushinteger(L, s->exit_code); lua_setfield(L, -2, "exit");
  }
  lua_pushstring(L, s->running ? (s->stopping ? "stopping" : "running") :
      (s->timed_out ? "timed_out" : "exited"));
  lua_setfield(L, -2, "status");
#undef PROCESS_STRING
#undef PROCESS_NUMBER
#undef PROCESS_BOOL
}

static int l_tools_processes(lua_State *L) {
  const char *action = luaL_optstring(L, 1, "list");
  process_manager_poll();
  if (strcmp(action, "list") == 0) {
    lua_newtable(L);
    lua_Integer n = 0;
    for (size_t i = 0; i < process_manager_count(); ++i) {
      ProcessSnapshot s;
      if (process_manager_at(i, &s) && process_visible(&s)) {
        push_process(L, &s);
        lua_rawseti(L, -2, ++n);
      }
    }
    return 1;
  }
  if (strcmp(action, "get") != 0 && strcmp(action, "output") != 0)
    return luaL_error(L, "unknown processes action");
  const char *id = luaL_checkstring(L, 2);
  ProcessSnapshot s;
  if (!process_manager_get(id, &s) || !process_visible(&s))
    return luaL_error(L, "process not found in this session");
  push_process(L, &s);
  if (strcmp(action, "get") == 0) {
    ProcessDescendant children[128];
    size_t count = s.running ? process_observe_descendants(s.pid, s.pgid, children, 128) : 0;
    lua_newtable(L);
    for (size_t i = 0; i < count; i++) {
      lua_newtable(L);
      lua_pushinteger(L, children[i].pid); lua_setfield(L, -2, "pid");
      lua_pushinteger(L, children[i].ppid); lua_setfield(L, -2, "ppid");
      lua_pushstring(L, children[i].name); lua_setfield(L, -2, "name");
      lua_pushboolean(L, children[i].in_managed_group); lua_setfield(L, -2, "in_managed_group");
      lua_pushboolean(L, 1); lua_setfield(L, -2, "observed_only");
      lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "descendants");
  }
  if (strcmp(action, "output") == 0) {
    for (int stream = 0; stream < 2; ++stream) {
      char *output = process_manager_output(id, stream);
      lua_pushstring(L, output ? output : "");
      free(output);
      lua_setfield(L, -2, stream ? "stderr" : "stdout");
    }
  }
  return 1;
}

static int l_tools_process_stop(lua_State *L) {
  const char *id = luaL_checkstring(L, 1);
  ProcessSnapshot s;
  if (!process_manager_get(id, &s) || !process_visible(&s))
    return luaL_error(L, "process not found in this session");
  /* The dispatcher authorizes the distinct process_stop permission before
   * this binding, including one-shot/ACP grants. Ownership is enforced here;
   * never reinterpret a shell/MCP grant as a process_stop grant. */
  if (!process_manager_stop(id))
    return luaL_error(L, "failed to request process stop");
  if (!process_manager_get(id, &s))
    return luaL_error(L, "process snapshot unavailable");
  push_process(L, &s);
  return 1;
}

static int l_tools_shell(lua_State *L) {
  const char *command = luaL_checkstring(L, 1);
  int background = lua_toboolean(L, 3);
  lua_Integer requested = luaL_optinteger(L, 2,
      background ? 0 : PERMIT_DEFAULT_SHELL_TIMEOUT);
  int timeout = background && requested > 300 ? 300 : (requested > 0 ? (int)requested :
      (background ? 0 : PERMIT_DEFAULT_SHELL_TIMEOUT));
  if (background) {
    char id[PROCESS_ID_SIZE];
    process_manager_set_owner(tools_process_owner());
    if (!process_manager_start(command, NULL, app_workdir(), timeout,
        PERMIT_MAX_STDOUT, PERMIT_MAX_STDERR, id))
      return luaL_error(L, "failed to start background shell process");
    ProcessSnapshot s;
    if (!process_manager_get(id, &s))
      return luaL_error(L, "background process snapshot unavailable");
    process_manager_watch(id);
    push_process(L, &s);
    lua_pushboolean(L, 1); lua_setfield(L, -2, "started");
    return 1;
  }

  process_manager_set_owner(tools_process_owner());
  long long started_ms = now_ms();
  log_shell_start(timeout, command);
  ShellProcessResult result;
  if (!shell_process_run(command, app_workdir(), timeout, PERMIT_MAX_STDOUT,
                         PERMIT_MAX_STDERR, tui_pump_blocking, &result)) {
    lua_newtable(L);
    lua_pushinteger(L, -1);
    lua_setfield(L, -2, "exit");
    lua_pushstring(L, "failed to start shell process");
    lua_setfield(L, -2, "stdout");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "stderr");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "timed_out");
    log_shell_done(-1, 0, now_ms() - started_ms, command);
    return 1;
  }

  lua_newtable(L);
  lua_pushinteger(L, result.exit_code);
  lua_setfield(L, -2, "exit");
  lua_pushstring(L, result.stdout_text);
  lua_setfield(L, -2, "stdout");
  lua_pushstring(L, result.stderr_text);
  lua_setfield(L, -2, "stderr");
  lua_pushboolean(L, result.timed_out);
  lua_setfield(L, -2, "timed_out");

  log_shell_done(result.exit_code, result.timed_out, now_ms() - started_ms,
                 command);
  shell_process_result_free(&result);

  return 1;
}

static int l_tools_exec(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  size_t count = lua_rawlen(L, 1);
  if (count == 0 || count > 128)
    return luaL_error(L, "exec argv must contain 1..128 strings");
  char **argv = calloc(count + 1, sizeof(*argv));
  if (!argv)
    return luaL_error(L, "out of memory");
  for (size_t i = 0; i < count; i++) {
    lua_rawgeti(L, 1, (lua_Integer)i + 1);
    if (lua_type(L, -1) != LUA_TSTRING) {
      free(argv);
      return luaL_error(L, "exec argv values must be strings");
    }
    argv[i] = (char *)lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  int timeout = (int)luaL_optinteger(L, 2, PERMIT_DEFAULT_SHELL_TIMEOUT);
  if (timeout <= 0)
    timeout = PERMIT_DEFAULT_SHELL_TIMEOUT;
  ShellProcessResult result;
  process_manager_set_owner(tools_process_owner());
  int started = shell_process_run_argv(argv, app_workspace_root(), timeout,
      PERMIT_MAX_STDOUT, PERMIT_MAX_STDERR, tui_pump_blocking, &result);
  free(argv);
  lua_newtable(L);
  lua_pushinteger(L, started ? result.exit_code : -1);
  lua_setfield(L, -2, "exit");
  lua_pushstring(L, started ? result.stdout_text : "");
  lua_setfield(L, -2, "stdout");
  lua_pushstring(L, started ? result.stderr_text : "failed to start process");
  lua_setfield(L, -2, "stderr");
  lua_pushboolean(L, started && result.timed_out);
  lua_setfield(L, -2, "timed_out");
  if (started)
    shell_process_result_free(&result);
  return 1;
}

/* Internal runtime operations, deliberately absent from model schemas. */
static int l_tools_process_events(lua_State *L) {
  const char *owner = luaL_checkstring(L, 1);
  process_manager_poll();
  lua_newtable(L);
  ProcessSnapshot s;
  int n = 0;
  while (process_manager_completion(owner, &s)) {
    push_process(L, &s);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
static int l_tools_process_close_owner(lua_State *L) {
  lua_pushboolean(L, process_manager_close_owner(luaL_checkstring(L, 1)));
  return 1;
}
void tools_init(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_tools_shell);
  lua_setfield(L, -2, "shell");
  lua_pushcfunction(L, l_tools_exec);
  lua_setfield(L, -2, "exec");
  lua_pushcfunction(L, l_tools_process_scope);
  lua_setfield(L, -2, "process_scope");
  lua_pushcfunction(L, l_tools_processes);
  lua_setfield(L, -2, "processes");
  lua_pushcfunction(L, l_tools_process_stop);
  lua_setfield(L, -2, "process_stop");
  lua_pushcfunction(L, l_tools_process_events);
  lua_setfield(L, -2, "process_events");
  lua_pushcfunction(L, l_tools_process_close_owner);
  lua_setfield(L, -2, "process_close_owner");
  lua_setglobal(L, "tools");
}
