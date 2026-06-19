#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>

static int mock_status = 200;
static const char *mock_body = "ok";
static char last_url[512];
static int http_get_calls = 0;

static int l_mock_http_get(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  strncpy(last_url, url, sizeof(last_url) - 1);
  last_url[sizeof(last_url) - 1] = '\0';
  http_get_calls++;
  lua_pushinteger(L, mock_status);
  lua_pushstring(L, mock_body);
  return 2;
}

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

  lua_newtable(L);
  lua_pushcfunction(L, l_mock_http_get);
  lua_setfield(L, -2, "get");
  lua_setglobal(L, "http");

  return L;
}

static void reset_mock(void) {
  mock_status = 200;
  mock_body = "ok";
  last_url[0] = '\0';
  http_get_calls = 0;
}

static void load_fetch_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/fetch.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void push_ctx(lua_State *L, const char *arg_url, const char *tool_url) {
  lua_newtable(L);

  lua_newtable(L);
  if (arg_url) {
    lua_pushstring(L, arg_url);
    lua_rawseti(L, -2, 1);
  }
  lua_setfield(L, -2, "args");

  if (tool_url) {
    lua_newtable(L);
    lua_pushstring(L, tool_url);
    lua_setfield(L, -2, "url");
    lua_setfield(L, -2, "tool_args");
  }

  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
}

static void call_handler(lua_State *L, const char *arg_url, const char *tool_url) {
  lua_getfield(L, -1, "handler");
  push_ctx(L, arg_url, tool_url);
  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_metadata(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_fetch_plugin(L);

  lua_getfield(L, -1, "id");
  munit_assert_string_equal(lua_tostring(L, -1), "fetch");
  lua_pop(L, 1);

  lua_getfield(L, -1, "command");
  munit_assert_string_equal(lua_tostring(L, -1), "/fetch");
  lua_pop(L, 1);

  lua_getfield(L, -1, "tool");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "name");
  munit_assert_string_equal(lua_tostring(L, -1), "fetch");
  lua_pop(L, 1);
  lua_getfield(L, -1, "permission");
  munit_assert_string_equal(lua_tostring(L, -1), "fetch");
  lua_pop(L, 1);
  lua_getfield(L, -1, "parameters");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "required");
  lua_rawgeti(L, -1, 1);
  munit_assert_string_equal(lua_tostring(L, -1), "url");

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_missing_url(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  reset_mock();
  lua_State *L = new_state();
  load_fetch_plugin(L);

  call_handler(L, NULL, NULL);

  munit_assert_string_equal(lua_tostring(L, -2), "Usage: /fetch <url>");
  munit_assert_string_equal(lua_tostring(L, -1), "Usage: /fetch <url>");
  munit_assert_int(http_get_calls, ==, 0);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_rejects_non_http_url(const MunitParameter params[],
                                             void *data) {
  (void)params;
  (void)data;
  reset_mock();
  lua_State *L = new_state();
  load_fetch_plugin(L);

  call_handler(L, "file:///etc/passwd", NULL);

  munit_assert_string_equal(lua_tostring(L, -2),
                            "Usage: /fetch <http-or-https-url>");
  munit_assert_int(http_get_calls, ==, 0);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_fetch_success(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  reset_mock();
  mock_status = 200;
  mock_body = "hello";
  lua_State *L = new_state();
  load_fetch_plugin(L);

  call_handler(L, "https://example.com/data", NULL);

  munit_assert_string_equal(last_url, "https://example.com/data");
  munit_assert_string_equal(lua_tostring(L, -2),
                            "Fetched https://example.com/data (HTTP 200, 5 bytes)");
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(llm, "URL: https://example.com/data") != NULL);
  munit_assert_true(strstr(llm, "Status: 200") != NULL);
  munit_assert_true(strstr(llm, "hello") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_adds_https_when_scheme_missing(const MunitParameter params[],
                                                       void *data) {
  (void)params;
  (void)data;
  reset_mock();
  mock_status = 200;
  mock_body = "normalized";
  lua_State *L = new_state();
  load_fetch_plugin(L);

  call_handler(L, "example.com/path", NULL);

  munit_assert_string_equal(last_url, "https://example.com/path");
  munit_assert_string_equal(lua_tostring(L, -2),
                            "Fetched https://example.com/path (HTTP 200, 10 bytes)");
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(llm, "URL: https://example.com/path") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_args_and_http_error(const MunitParameter params[],
                                                 void *data) {
  (void)params;
  (void)data;
  reset_mock();
  mock_status = 404;
  mock_body = "missing";
  lua_State *L = new_state();
  load_fetch_plugin(L);

  call_handler(L, NULL, "http://example.test/missing");

  munit_assert_string_equal(last_url, "http://example.test/missing");
  munit_assert_string_equal(lua_tostring(L, -2),
                            "Fetch failed: http://example.test/missing (HTTP 404)");
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(llm, "Status: 404") != NULL);
  munit_assert_true(strstr(llm, "missing") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/metadata", test_metadata, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/missing_url", test_missing_url, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_non_http_url", test_rejects_non_http_url, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/success", test_fetch_success, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/adds_https_when_scheme_missing", test_adds_https_when_scheme_missing, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_args_http_error", test_tool_args_and_http_error, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite fetch_plugin_suite = {"/fetch_plugin", tests, NULL, 1,
                                 MUNIT_SUITE_OPTION_NONE};
