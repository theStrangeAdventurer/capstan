#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  return L;
}

static void load_file_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L, const char *path) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_readme_fallback_reads_readme_md(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_file_plugin(L);

  call_handler(L, "README");

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, "README.md") != NULL);
  munit_assert_true(strstr(llm, "README.md") != NULL);
  munit_assert_true(strstr(llm, "# tui-agent") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/readme_fallback_reads_readme_md", test_readme_fallback_reads_readme_md,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite file_plugin_suite = {"/file_plugin", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
