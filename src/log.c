#include "log.h"
#include "app_config.h"
#include "redact.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_MAX_BYTES (10L * 1024L * 1024L)
#define LOG_MAX_ARCHIVES 5

static lua_State *g_log_lua = NULL;

int log_path(char *buf, size_t buf_size) {
  time_t now = time(NULL);
  struct tm tm_buf;
  struct tm *tm = localtime_r(&now, &tm_buf);
  char name[64];
  if (tm) {
    if (strftime(name, sizeof(name), "logs/%Y-%m-%d.log", tm) == 0)
      return -1;
  } else {
    snprintf(name, sizeof(name), "logs/unknown-date.log");
  }
  return app_state_path(buf, buf_size, name);
}

static int ensure_log_dir(void) {
  char dir[512];
  if (app_state_path(dir, sizeof(dir), "logs") != 0)
    return -1;

  struct stat st;
  if (stat(dir, &st) == 0)
    return S_ISDIR(st.st_mode) ? 0 : -1;

  return mkdir(dir, 0755);
}

static void sanitize(char *s) {
  for (; *s; s++) {
    if (*s == '\n' || *s == '\r' || *s == '\t')
      *s = ' ';
  }
}

static char *dup_string(const char *s) {
  size_t len = strlen(s);
  char *copy = malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, s, len + 1);
  return copy;
}

static char *lua_redact_alloc(const char *message) {
  if (!g_log_lua)
    return NULL;

  lua_State *l = g_log_lua;
  int top = lua_gettop(l);

  lua_getglobal(l, "require");
  if (!lua_isfunction(l, -1)) {
    lua_settop(l, top);
    return NULL;
  }
  lua_pushstring(l, "agent.redact");
  if (lua_pcall(l, 1, 1, 0) != LUA_OK) {
    lua_settop(l, top);
    return NULL;
  }
  if (!lua_istable(l, -1)) {
    lua_settop(l, top);
    return NULL;
  }

  lua_getfield(l, -1, "text");
  if (!lua_isfunction(l, -1)) {
    lua_settop(l, top);
    return NULL;
  }
  lua_pushstring(l, message ? message : "");
  if (lua_pcall(l, 1, 1, 0) != LUA_OK) {
    lua_settop(l, top);
    return NULL;
  }

  const char *redacted = lua_tostring(l, -1);
  char *copy = dup_string(redacted ? redacted : "");
  lua_settop(l, top);
  return copy;
}

static int rotated_path(const char *path, int archive_index, char *buf,
                        size_t buf_size) {
  const char *suffix = ".log";
  size_t path_len = strlen(path);
  size_t suffix_len = strlen(suffix);
  if (path_len < suffix_len ||
      strcmp(path + path_len - suffix_len, suffix) != 0)
    return -1;

  int n = snprintf(buf, buf_size, "%.*s.%d.log",
                   (int)(path_len - suffix_len), path, archive_index);
  return n < 0 || (size_t)n >= buf_size ? -1 : 0;
}

static void rotate_if_needed(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size < LOG_MAX_BYTES)
    return;

  char old_path[512];
  char new_path[512];
  if (rotated_path(path, LOG_MAX_ARCHIVES, old_path, sizeof(old_path)) == 0)
    remove(old_path);

  for (int i = LOG_MAX_ARCHIVES - 1; i >= 1; i--) {
    if (rotated_path(path, i, old_path, sizeof(old_path)) != 0 ||
        rotated_path(path, i + 1, new_path, sizeof(new_path)) != 0)
      continue;
    rename(old_path, new_path);
  }

  if (rotated_path(path, 1, new_path, sizeof(new_path)) == 0)
    rename(path, new_path);
}

void log_event(const char *category, const char *message) {
  char path[512];
  if (app_state_ensure_dir() != 0 || ensure_log_dir() != 0 ||
      log_path(path, sizeof(path)) != 0)
    return;

  rotate_if_needed(path);

  FILE *f = fopen(path, "a");
  if (!f)
    return;

  time_t now = time(NULL);
  struct tm tm_buf;
  struct tm *tm = localtime_r(&now, &tm_buf);
  char ts[32];
  if (tm)
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
  else
    snprintf(ts, sizeof(ts), "unknown-time");

  char cat[64];
  snprintf(cat, sizeof(cat), "%s", category ? category : "event");
  sanitize(cat);

  char *redacted = lua_redact_alloc(message ? message : "");
  if (!redacted)
    redacted = redact_secrets_alloc(message ? message : "");
  if (!redacted)
    redacted = dup_string("[REDACTION_FAILED]");
  if (!redacted) {
    fclose(f);
    return;
  }
  sanitize(redacted);

  fprintf(f, "%s [%s] %s\n", ts, cat, redacted);
  free(redacted);
  fclose(f);
}

static int l_capstan_log(lua_State *L) {
  const char *category = luaL_checkstring(L, 1);
  const char *message = luaL_optstring(L, 2, "");
  log_event(category, message);
  return 0;
}

static int l_capstan_log_path(lua_State *L) {
  char path[512];
  if (log_path(path, sizeof(path)) != 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, path);
  return 1;
}

void log_init(lua_State *L) {
  g_log_lua = L;

  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }

  lua_pushcfunction(L, l_capstan_log);
  lua_setfield(L, -2, "log");

  lua_pushcfunction(L, l_capstan_log_path);
  lua_setfield(L, -2, "log_path");

  lua_setglobal(L, "capstan");
}

void log_cleanup(void) { g_log_lua = NULL; }
