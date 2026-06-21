#include "log.h"
#include "app_config.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int log_path(char *buf, size_t buf_size) {
  return app_config_path(buf, buf_size, "events.log");
}

static void sanitize(char *s) {
  for (; *s; s++) {
    if (*s == '\n' || *s == '\r' || *s == '\t')
      *s = ' ';
  }
}

void log_event(const char *category, const char *message) {
  char path[512];
  if (app_config_ensure_dir() != 0 || log_path(path, sizeof(path)) != 0)
    return;

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

  char msg[2048];
  snprintf(msg, sizeof(msg), "%s", message ? message : "");
  sanitize(msg);

  fprintf(f, "%s [%s] %s\n", ts, cat, msg);
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
