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

static lua_State *new_state(const char *summary) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_pushstring(L, summary);
  lua_setfield(L, -2, "skills_summary");
  lua_setglobal(L, "capstan");

  return L;
}

static void load_skills_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/skills.lua");
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

static MunitResult test_skills_plugin_returns_summary(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state("Loaded skills: 1\n- code-review [project]");
  load_skills_plugin(L);
  call_handler(L);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, "code-review") != NULL);
  munit_assert_string_equal(ui, llm);

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/returns_summary", test_skills_plugin_returns_summary, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite skills_plugin_suite = {"/skills_plugin", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
