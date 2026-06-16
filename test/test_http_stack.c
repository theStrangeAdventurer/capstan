#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>

static int callback_ok(lua_State *L) {
  lua_pushstring(L, "ok");
  return 1;
}

static int callback_err(lua_State *L) {
  lua_pushstring(L, "intentional crash for testing");
  lua_error(L);
  return 0;
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  return L;
}

static MunitResult test_success_stable_stack(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  lua_State *L = new_state();

  for (int i = 0; i < 100; i++) {
    munit_assert_int(lua_gettop(L), ==, 0);

    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);
    lua_pushcfunction(L, callback_ok);
    lua_pushinteger(L, 123);

    int msgh = lua_gettop(L) - 2;
    int rc = lua_pcall(L, 1, 0, msgh);
    munit_assert_int(rc, ==, LUA_OK);

    munit_assert_int(lua_gettop(L), <=, 2);
    lua_settop(L, 0);
  }

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_error_gives_traceback(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  lua_State *L = new_state();

  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");
  lua_remove(L, -2);
  lua_pushcfunction(L, callback_err);
  lua_pushinteger(L, 456);

  int msgh = lua_gettop(L) - 2;
  int rc = lua_pcall(L, 1, 0, msgh);
  munit_assert_int(rc, !=, LUA_OK);

  munit_assert_int(lua_gettop(L), >=, 1);
  const char *tb = lua_tostring(L, -1);
  munit_assert_not_null(tb);
  munit_assert_true(strstr(tb, "stack traceback") != NULL);
  munit_assert_true(strstr(tb, "intentional crash") != NULL);

  lua_settop(L, 0);
  munit_assert_int(lua_gettop(L), ==, 0);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_repeated_no_stack_growth(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  lua_State *L = new_state();

  for (int round = 0; round < 10; round++) {
    for (int i = 0; i < 10; i++) {
      munit_assert_int(lua_gettop(L), ==, 0);
      lua_getglobal(L, "debug");
      lua_getfield(L, -1, "traceback");
      lua_remove(L, -2);
      lua_pushcfunction(L, callback_ok);
      lua_pushinteger(L, i);

      int msgh = lua_gettop(L) - 2;
      int rc = lua_pcall(L, 1, 0, msgh);
      munit_assert_int(rc, ==, LUA_OK);
      lua_settop(L, 0);
      munit_assert_int(lua_gettop(L), ==, 0);
    }

    munit_assert_int(lua_gettop(L), ==, 0);
    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);
    lua_pushcfunction(L, callback_err);
    lua_pushinteger(L, round);

    int msgh = lua_gettop(L) - 2;
    int rc = lua_pcall(L, 1, 0, msgh);
    munit_assert_int(rc, !=, LUA_OK);
    munit_assert_int(lua_gettop(L), >=, 1);
    lua_settop(L, 0);
    munit_assert_int(lua_gettop(L), ==, 0);
  }

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/stable", test_success_stable_stack, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/traceback", test_error_gives_traceback, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/no_growth", test_repeated_no_stack_growth, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite http_stack_suite = {"/stack", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
