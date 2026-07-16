#include "app_config.h"
#include "dyn_arr.h"
#include "log.h"
#include "permit.h"
#include "shell_process.h"
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

static int l_tools_shell(lua_State *L) {
  const char *command = luaL_checkstring(L, 1);
  int timeout = PERMIT_DEFAULT_SHELL_TIMEOUT;
  if (lua_gettop(L) >= 2)
    timeout = (int)luaL_checkinteger(L, 2);
  if (timeout <= 0)
    timeout = PERMIT_DEFAULT_SHELL_TIMEOUT;

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

void tools_init(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_tools_shell);
  lua_setfield(L, -2, "shell");
  lua_setglobal(L, "tools");
}
