#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char temp_log_path[512];

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static int l_log_path(lua_State *L) {
  lua_pushstring(L, temp_log_path);
  return 1;
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_pushcfunction(L, l_log_path);
  lua_setfield(L, -2, "log_path");
  lua_setglobal(L, "capstan");

  return L;
}

static void write_temp_log(void) {
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-log-%ld.txt", (long)getpid());
  FILE *f = fopen(temp_log_path, "w");
  munit_assert_not_null(f);
  fputs("one\n", f);
  fputs("two\n", f);
  fputs("three\n", f);
  fclose(f);
}

static void load_logs_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/logs.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L, const char *limit) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  if (limit) {
    lua_pushstring(L, limit);
    lua_rawseti(L, -2, 1);
  }
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_logs_tail_limit(const MunitParameter params[],
                                        void *data) {
  (void)params;
  (void)data;
  write_temp_log();
  lua_State *L = new_state();
  load_logs_plugin(L);

  call_handler(L, "2");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, temp_log_path) != NULL);
  munit_assert_true(strstr(ui, "one") == NULL);
  munit_assert_true(strstr(ui, "two") != NULL);
  munit_assert_true(strstr(ui, "three") != NULL);

  lua_close(L);
  unlink(temp_log_path);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/tail_limit", test_logs_tail_limit, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite logs_plugin_suite = {"/logs_plugin", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
