#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static int l_const_string(lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int l_path_join(lua_State *L) {
  const char *base = lua_tostring(L, lua_upvalueindex(1));
  const char *relative = luaL_optstring(L, 1, "");
  lua_pushfstring(L, "%s/%s", base, relative);
  return 1;
}

static int l_weak_model(lua_State *L) {
  lua_newtable(L);
  lua_pushstring(L, "openrouter");
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, "minimax/minimax-m3");
  lua_setfield(L, -2, "model");
  return 1;
}

static void set_const_string(lua_State *L, const char *name,
                             const char *value) {
  lua_pushstring(L, value);
  lua_pushcclosure(L, l_const_string, 1);
  lua_setfield(L, -2, name);
}

static void set_path_join(lua_State *L, const char *name, const char *base) {
  lua_pushstring(L, base);
  lua_pushcclosure(L, l_path_join, 1);
  lua_setfield(L, -2, name);
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_pushstring(L, "/work/project");
  lua_setfield(L, -2, "workdir");
  set_const_string(L, "config_dir", "/home/me/.config/capstan");
  set_const_string(L, "state_dir", "/home/me/.local/state/capstan");
  set_const_string(L, "log_path",
                   "/home/me/.local/state/capstan/logs/2026-06-27.log");
  set_path_join(L, "config_path", "/home/me/.config/capstan");
  set_path_join(L, "state_path", "/home/me/.local/state/capstan");

  lua_newtable(L);
  set_const_string(L, "current_provider", "deepseek");
  set_const_string(L, "current_model", "deepseek-chat");
  lua_pushcfunction(L, l_weak_model);
  lua_setfield(L, -2, "weak");
  lua_setfield(L, -2, "models");
  lua_setglobal(L, "capstan");

  return L;
}

static void load_info_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/info.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_info_is_popup_only_command(const MunitParameter params[],
                                                   void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_info_plugin(L);

  lua_getfield(L, -1, "history");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);

  lua_getfield(L, -1, "command");
  munit_assert_string_equal(lua_tostring(L, -1), "/info");
  lua_pop(L, 1);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_info_includes_runtime_paths(const MunitParameter params[],
                                                    void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_info_plugin(L);

  call_handler(L);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_string_equal(ui, llm);
  munit_assert_true(strstr(ui, "workdir: /work/project") != NULL);
  munit_assert_true(strstr(ui, "config dir: /home/me/.config/capstan") != NULL);
  munit_assert_true(strstr(ui, "config file: /home/me/.config/capstan/config.lua") != NULL);
  munit_assert_true(strstr(ui, "project skills: /work/project/.agents/skills") != NULL);
  munit_assert_true(strstr(ui, "user config skills: /home/me/.config/capstan/skills") != NULL);
  munit_assert_true(strstr(ui, "builtin skills: embedded:skills") != NULL);
  munit_assert_true(strstr(ui, "permissions: /home/me/.local/state/capstan/permissions.lua") != NULL);
  munit_assert_true(strstr(ui, "current log: /home/me/.local/state/capstan/logs/2026-06-27.log") != NULL);
  munit_assert_true(strstr(ui, "provider: deepseek") != NULL);
  munit_assert_true(strstr(ui, "model: deepseek-chat") != NULL);
  munit_assert_true(strstr(ui, "weak model: openrouter/minimax/minimax-m3") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/popup_only_command", test_info_is_popup_only_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/runtime_paths", test_info_includes_runtime_paths, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite info_plugin_suite = {"/info_plugin", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
