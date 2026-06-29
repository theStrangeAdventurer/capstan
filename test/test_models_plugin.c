#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>

static char selected_model[128];
static char selected_provider[128];
static char selected_weak_model[128];
static char selected_weak_provider[128];
static char selected_profile[128];
static char selected_profile_model[128];
static char selected_profile_provider[128];

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

static int l_models_list_all(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "openrouter");
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, "model/a");
  lua_setfield(L, -2, "id");
  lua_pushstring(L, "openrouter/model/a  Model A");
  lua_setfield(L, -2, "text");
  lua_rawseti(L, -2, 1);
  lua_newtable(L);
  lua_pushstring(L, "deepseek");
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, "deepseek-chat");
  lua_setfield(L, -2, "id");
  lua_pushstring(L, "deepseek/deepseek-chat");
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

static int l_models_set_for(lua_State *L) {
  const char *provider = luaL_checkstring(L, 1);
  const char *model = luaL_checkstring(L, 2);
  strncpy(selected_provider, provider, sizeof(selected_provider) - 1);
  selected_provider[sizeof(selected_provider) - 1] = '\0';
  strncpy(selected_model, model, sizeof(selected_model) - 1);
  selected_model[sizeof(selected_model) - 1] = '\0';
  lua_pushboolean(L, 1);
  return 1;
}

static int l_models_set_weak(lua_State *L) {
  const char *provider = luaL_checkstring(L, 1);
  const char *model = luaL_checkstring(L, 2);
  strncpy(selected_weak_provider, provider, sizeof(selected_weak_provider) - 1);
  selected_weak_provider[sizeof(selected_weak_provider) - 1] = '\0';
  strncpy(selected_weak_model, model, sizeof(selected_weak_model) - 1);
  selected_weak_model[sizeof(selected_weak_model) - 1] = '\0';
  lua_pushboolean(L, 1);
  return 1;
}

static int l_models_set_profile(lua_State *L) {
  const char *profile = luaL_checkstring(L, 1);
  const char *provider = luaL_checkstring(L, 2);
  const char *model = luaL_checkstring(L, 3);
  strncpy(selected_profile, profile, sizeof(selected_profile) - 1);
  selected_profile[sizeof(selected_profile) - 1] = '\0';
  strncpy(selected_profile_provider, provider,
          sizeof(selected_profile_provider) - 1);
  selected_profile_provider[sizeof(selected_profile_provider) - 1] = '\0';
  strncpy(selected_profile_model, model, sizeof(selected_profile_model) - 1);
  selected_profile_model[sizeof(selected_profile_model) - 1] = '\0';
  lua_pushboolean(L, 1);
  return 1;
}

static int l_current_provider(lua_State *L) {
  lua_pushstring(L, "openrouter");
  return 1;
}

static lua_State *new_state(void) {
  selected_model[0] = '\0';
  selected_provider[0] = '\0';
  selected_weak_model[0] = '\0';
  selected_weak_provider[0] = '\0';
  selected_profile[0] = '\0';
  selected_profile_model[0] = '\0';
  selected_profile_provider[0] = '\0';
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, l_models_list);
  lua_setfield(L, -2, "list");
  lua_pushcfunction(L, l_models_list_all);
  lua_setfield(L, -2, "list_all");
  lua_pushcfunction(L, l_models_set);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, l_models_set_for);
  lua_setfield(L, -2, "set_for");
  lua_pushcfunction(L, l_models_set_weak);
  lua_setfield(L, -2, "set_weak");
  lua_pushcfunction(L, l_models_set_profile);
  lua_setfield(L, -2, "set_profile");
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
  munit_assert_string_equal(lua_tostring(L, -2),
                            "main  deepseek/deepseek-chat");
  munit_assert_string_equal(lua_tostring(L, -1),
                            "primary\tdeepseek\tdeepseek-chat");

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_autocomplete_marks_weak_mode(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "autocomplete");
  lua_getfield(L, -1, "fetch");
  lua_newtable(L);
  lua_pushstring(L, "--weak");
  lua_rawseti(L, -2, 1);
  int rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_rawgeti(L, -1, 1);
  lua_getfield(L, -1, "text");
  lua_getfield(L, -2, "value");
  munit_assert_true(strstr(lua_tostring(L, -2), "weak  ") != NULL);
  munit_assert_true(strstr(lua_tostring(L, -1), "weak\t") == lua_tostring(L, -1));

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_autocomplete_marks_profile_mode(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "autocomplete");
  lua_getfield(L, -1, "fetch");
  lua_newtable(L);
  lua_pushstring(L, "--profile");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "plan");
  lua_rawseti(L, -2, 2);
  int rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_rawgeti(L, -1, 1);
  lua_getfield(L, -1, "text");
  lua_getfield(L, -2, "value");
  munit_assert_true(strstr(lua_tostring(L, -2), "plan  ") != NULL);
  munit_assert_true(strstr(lua_tostring(L, -1), "profile\tplan\t") ==
                    lua_tostring(L, -1));

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

static MunitResult test_handler_sets_provider_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "handler");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "deepseek");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "deepseek-chat");
  lua_rawseti(L, -2, 2);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(selected_provider, "deepseek");
  munit_assert_string_equal(selected_model, "deepseek-chat");
  munit_assert_true(strstr(lua_tostring(L, -2), "deepseek/deepseek-chat") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_handler_sets_weak_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "handler");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "--weak");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "openrouter");
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, "minimax/minimax-m3");
  lua_rawseti(L, -2, 3);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(selected_weak_provider, "openrouter");
  munit_assert_string_equal(selected_weak_model, "minimax/minimax-m3");
  munit_assert_true(strstr(lua_tostring(L, -2),
                           "Weak model set: openrouter/minimax/minimax-m3") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_handler_sets_profile_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_models_plugin(L);

  lua_getfield(L, -1, "handler");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "--profile");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "plan");
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, "openrouter");
  lua_rawseti(L, -2, 3);
  lua_pushstring(L, "planner/model");
  lua_rawseti(L, -2, 4);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(selected_profile, "plan");
  munit_assert_string_equal(selected_profile_provider, "openrouter");
  munit_assert_string_equal(selected_profile_model, "planner/model");
  munit_assert_true(strstr(lua_tostring(L, -2),
                           "Profile model set: plan openrouter/planner/model") != NULL);

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
    {"/autocomplete_marks_weak_mode", test_autocomplete_marks_weak_mode, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/autocomplete_marks_profile_mode", test_autocomplete_marks_profile_mode,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/handler_sets_runtime_model", test_handler_sets_runtime_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/handler_sets_provider_model", test_handler_sets_provider_model, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/handler_sets_weak_model", test_handler_sets_weak_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/handler_sets_profile_model", test_handler_sets_profile_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/models_plugin_is_no_history_command",
     test_models_plugin_is_no_history_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite models_plugin_suite = {"/models_plugin", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
