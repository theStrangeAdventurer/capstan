#include "munit.h"
#include "http.h"
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

static MunitResult test_stream_outlives_coroutine(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  lua_State *L = new_state();
  http_init(L);
  http_set_headless(1);
  const char *script =
      "main_thread = coroutine.running(); calls = 0; "
      "weak = setmetatable({}, {__mode='v'}); "
      "local co = coroutine.create(function() "
      "  request_id = http.post_stream('http://127.0.0.1:1/', '{}', {}, function(raw, done) "
      "    assert(coroutine.running() == main_thread); "
      "    local child = coroutine.create(function() return raw end); "
      "    local ok, result = coroutine.resume(child); assert(ok and result == 'fixture'); "
      "    calls = calls + 1 "
      "  end) "
      "end); weak[1] = co; assert(coroutine.resume(co)); co = nil; "
      "collectgarbage('collect'); collectgarbage('collect'); assert(weak[1] == nil)";
  munit_assert_int(luaL_dostring(L, script), ==, LUA_OK);
  lua_getglobal(L, "request_id");
  int id = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  munit_assert_true(http_test_deliver_chunk(id, "fixture"));
  munit_assert_int(luaL_dostring(L, "assert(calls == 1); assert(http.cancel(request_id)); collectgarbage('collect')"), ==, LUA_OK);
  munit_assert_int(lua_gettop(L), ==, 0);
  http_cleanup();
  http_set_headless(0);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_completion_from_coroutine_poll(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  lua_State *L = new_state();
  http_init(L);
  http_set_headless(1);
  /* file:// is rejected by the production protocol allowlist, so curl's real
     completion path can be exercised without DNS or a listening socket. */
  const char *script =
      "local main = coroutine.running(); local calls = 0; "
      "local origin = coroutine.create(function() "
      "  http.post_stream('file:///unused', '{}', {}, function(raw, done, err) "
      "    assert(coroutine.running() == main); assert(done and err); calls = calls + 1 "
      "  end); "
      "  http.post_response_async('file:///unused', '{}', {}, 100, function(response, err) "
      "    assert(coroutine.running() == main); assert(response == nil and err); calls = calls + 1 "
      "  end) "
      "end); assert(coroutine.resume(origin)); origin = nil; collectgarbage('collect'); "
      "local pump = coroutine.create(function() for i=1,10 do http.poll() end end); "
      "assert(coroutine.resume(pump)); assert(calls == 2); assert(not http.is_loading())";
  int rc = luaL_dostring(L, script);
  if (rc != LUA_OK) munit_logf(MUNIT_LOG_ERROR, "%s", lua_tostring(L, -1));
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(lua_gettop(L), ==, 0);
  http_cleanup();
  http_set_headless(0);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_easy_init_failure(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  lua_State *L = new_state();
  http_init(L);
  const char *calls[] = {
      "http.get('http://127.0.0.1:1/', {Accept='text/plain'})",
      "http.post('http://127.0.0.1:1/', '{}', {Accept='text/plain'})",
      "http.post_response('http://127.0.0.1:1/', '{}', {Accept='text/plain'})",
      "http.delete_response('http://127.0.0.1:1/', {Accept='text/plain'})",
      "http.post_stream('http://127.0.0.1:1/', '{}', {Accept='text/plain'}, function() end)",
      "http.post_response_async('http://127.0.0.1:1/', '{}', {Accept='text/plain'}, 100, function() end)"
  };
  for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
    http_test_fail_next(HTTP_TEST_FAIL_EASY_INIT);
    munit_assert_int(luaL_dostring(L, calls[i]), !=, LUA_OK);
    munit_assert_not_null(strstr(lua_tostring(L, -1),
                               "failed to initialize curl easy handle"));
    lua_pop(L, 1);
    munit_assert_false(http_is_loading());
    munit_assert_false(http_has_background_work());
    munit_assert_int(lua_gettop(L), ==, 0);
  }
  http_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_async_registration_failure(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  for (int response_mode = 0; response_mode <= 1; response_mode++) {
    for (int failure = HTTP_TEST_FAIL_EASY_INIT;
         failure <= HTTP_TEST_FAIL_ADD_HANDLE; failure++) {
      lua_State *L = new_state();
      http_init(L);
      munit_assert_int(luaL_dostring(L, response_mode ?
          "function submit(cb) return http.post_response_async('file:///unused', 'a\\0b', {Accept='text/plain'}, 100, cb, {background=true}) end" :
          "function submit(cb) return http.post_stream('file:///unused', 'a\\0b', {Accept='text/plain'}, cb, 100, {background=true}) end"), ==, LUA_OK);
      http_test_fail_next(failure);
      const char *script =
          "local weak = setmetatable({}, {__mode='v'}); "
          "local cb = function() end; weak[1] = cb; "
          "local ok, err = pcall(submit, cb); assert(not ok and type(err) == 'string'); "
          "cb = nil; collectgarbage('collect'); assert(weak[1] == nil); "
          "assert(not http.is_loading()); "
          "local calls = 0; local first = submit(function() calls = calls + 1 end); "
          "local second = submit(function() calls = calls + 1 end); "
          "assert(first == 1 and second == 2); "
          "assert(http.cancel(first)); assert(http.cancel(second)); "
          "assert(not http.cancel(first)); assert(calls == 0)";
      int rc = luaL_dostring(L, script);
      if (rc != LUA_OK) munit_logf(MUNIT_LOG_ERROR, "%s", lua_tostring(L, -1));
      munit_assert_int(rc, ==, LUA_OK);
      munit_assert_false(http_has_background_work());
      munit_assert_int(lua_gettop(L), ==, 0);
      http_cleanup();
      lua_close(L);
    }
  }
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/easy_init_failure", test_easy_init_failure, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/async_registration_failure", test_async_registration_failure, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/completion_from_coroutine_poll", test_completion_from_coroutine_poll, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_outlives_coroutine", test_stream_outlives_coroutine, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/stable", test_success_stable_stack, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/traceback", test_error_gives_traceback, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/no_growth", test_repeated_no_stack_growth, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite http_stack_suite = {"/stack", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
