#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>

static char selected_model[128];

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static int l_models_list(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "model/a");
  lua_setfield(L, -2, "id");
  lua_pushstring(L, "model/a  Model A");
  lua_setfield(L, -2, "text");
  lua_rawseti(L, -2, 1);
  lua_newtable(L);
  lua_pushstring(L, "model/b");
  lua_setfield(L, -2, "id");
  lua_pushstring(L, "model/b  Model B");
  lua_setfield(L, -2, "text");
  lua_rawseti(L, -2, 2);
  return 1;
}

static int l_models_set(lua_State *L) {
  const char *model = luaL_checkstring(L, 1);
  strncpy(selected_model, model, sizeof(selected_model) - 1);
  selected_model[sizeof(selected_model) - 1] = '\0';
  lua_pushboolean(L, 1);
  return 1;
}

static int l_current_provider(lua_State *L) {
  lua_pushstring(L, "openrouter");
  return 1;
}

static lua_State *new_state(void) {
  selected_model[0] = '\0';
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, l_models_list);
  lua_setfield(L, -2, "list");
  lua_pushcfunction(L, l_models_set);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, l_current_provider);
  lua_setfield(L, -2, "current_provider");
  lua_setfield(L, -2, "models");
  lua_setglobal(L, "capstan");

  return L;
}

static void load_models_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/models.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static MunitResult test_autocomplete_lists_runtime_models(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "autocomplete");
  lua_getfield(L, -1, "fetch");
  lua_newtable(L);
  int rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
  munit_assert_int((int)lua_rawlen(L, -1), ==, 2);
  lua_rawgeti(L, -1, 2);
  lua_getfield(L, -1, "text");
  lua_getfield(L, -2, "value");
  munit_assert_string_equal(lua_tostring(L, -2), "model/b  Model B");
  munit_assert_string_equal(lua_tostring(L, -1), "model/b");

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_handler_sets_runtime_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "handler");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "model/b");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(selected_model, "model/b");
  munit_assert_true(strstr(lua_tostring(L, -2), "openrouter/model/b") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_models_plugin_is_no_history_command(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "history");
  munit_assert_true(lua_isboolean(L, -1));
  munit_assert_false(lua_toboolean(L, -1));

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/autocomplete_lists_runtime_models", test_autocomplete_lists_runtime_models,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/handler_sets_runtime_model", test_handler_sets_runtime_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/models_plugin_is_no_history_command",
     test_models_plugin_is_no_history_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite models_plugin_suite = {"/models_plugin", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
