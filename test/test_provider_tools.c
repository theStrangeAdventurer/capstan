#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>

static int stream_callback_ref = LUA_NOREF;
static char captured_body[8192];
static char captured_permit_tool[128];
static char captured_permit_target[512];
static char captured_logs[8192];

static int l_http_get(lua_State *L) {
  (void)L;
  lua_pushinteger(L, 500);
  lua_pushstring(L, "");
  return 2;
}

static int l_http_post_stream(lua_State *L) {
  size_t body_len = 0;
  const char *body = luaL_checklstring(L, 2, &body_len);
  size_t copy = body_len < sizeof(captured_body) - 1
                    ? body_len
                    : sizeof(captured_body) - 1;
  memcpy(captured_body, body, copy);
  captured_body[copy] = '\0';

  if (stream_callback_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, stream_callback_ref);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  lua_pushvalue(L, 4);
  stream_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_pushinteger(L, 1);
  return 1;
}

static int l_permit_check(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *target = luaL_checkstring(L, 2);
  strncpy(captured_permit_tool, tool, sizeof(captured_permit_tool) - 1);
  captured_permit_tool[sizeof(captured_permit_tool) - 1] = '\0';
  strncpy(captured_permit_target, target, sizeof(captured_permit_target) - 1);
  captured_permit_target[sizeof(captured_permit_target) - 1] = '\0';
  lua_pushstring(L, "deny");
  return 1;
}

static int l_noop(lua_State *L) {
  (void)L;
  return 0;
}

static int l_capstan_log(lua_State *L) {
  const char *category = luaL_checkstring(L, 1);
  const char *message = luaL_optstring(L, 2, "");
  size_t used = strlen(captured_logs);
  if (used < sizeof(captured_logs) - 1) {
    snprintf(captured_logs + used, sizeof(captured_logs) - used, "[%s] %s\n",
             category, message);
  }
  return 0;
}

static lua_State *new_provider_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  luaL_dostring(L,
                "package.path = './?.lua;./?/init.lua;vendor/rxi/?.lua;' .. "
                "package.path");

  lua_newtable(L);
  lua_pushcfunction(L, l_http_get);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, l_http_post_stream);
  lua_setfield(L, -2, "post_stream");
  lua_setglobal(L, "http");

  lua_newtable(L);
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "append");
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "set_info");
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "set_usage");
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "set_thinking");
  lua_setglobal(L, "agent");

  lua_newtable(L);
  lua_pushcfunction(L, l_permit_check);
  lua_setfield(L, -2, "check");
  lua_setglobal(L, "permit");

  lua_newtable(L);
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "error");
  lua_setglobal(L, "popup");

  lua_newtable(L);
  lua_pushcfunction(L, l_capstan_log);
  lua_setfield(L, -2, "log");
  lua_setglobal(L, "capstan");

  luaL_dostring(L,
                "plugins = {"
                "fetch = {"
                "tool = {"
                "name = 'fetch',"
                "description = 'Fetch an HTTP or HTTPS URL',"
                "parameters = {"
                "type = 'object',"
                "properties = {url = {type = 'string'}},"
                "required = {'url'}"
                "}"
                "},"
                "handler = function(ctx) return ctx:replace('ui', 'llm') end"
                "},"
                "file = {"
                "tool = {"
                "name = 'file_read',"
                "description = 'Read a local file',"
                "parameters = {"
                "type = 'object',"
                "properties = {path = {type = 'string'}},"
                "required = {'path'}"
                "}"
                "},"
                "handler = function(ctx) return ctx:replace('file ui', 'file llm') end"
                "}"
                "}");

  return L;
}

static void reset_captures(lua_State *L) {
  if (stream_callback_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, stream_callback_ref);
    stream_callback_ref = LUA_NOREF;
  }
  captured_body[0] = '\0';
  captured_permit_tool[0] = '\0';
  captured_permit_target[0] = '\0';
  captured_logs[0] = '\0';
}

static void call_on_messages(lua_State *L) {
  lua_getglobal(L, "on_messages");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "Fetch https://example.com");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  int rc = lua_pcall(L, 1, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_request_enables_auto_tool_choice(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "ai/providers.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_on_messages(L);

  munit_assert_true(strstr(captured_body, "\"tools\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"fetch\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"file_read\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"tool_choice\":\"auto\"") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] request") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] tools=fetch,file_read") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] last_message role=user") != NULL);
  munit_assert_true(strstr(captured_logs, "[api] post_stream") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_fetch_permission_target_uses_url(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "ai/providers.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_on_messages(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                 "\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_1\",\"function\":{\"name\":\"fetch\","
                 "\"arguments\":\"{\\\"url\\\":\\\"https://example.com\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "fetch");
  munit_assert_string_equal(captured_permit_target, "https://example.com");
  munit_assert_true(strstr(captured_logs, "[tools] received 1 tool call") != NULL);
  munit_assert_true(strstr(captured_logs, "[tool] call name=fetch") != NULL);
  munit_assert_true(strstr(captured_logs, "[permit] tool=fetch") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_logs_text_when_model_does_not_call_tool(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "ai/providers.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_on_messages(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"I will look at it.\"}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_true(strstr(captured_logs, "[stream] done") != NULL);
  munit_assert_true(strstr(captured_logs, "final_tool_calls=0") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] stream done without tool calls text=I will look at it.") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_file_read_permission_target_uses_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "ai/providers.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_on_messages(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                 "\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_2\",\"function\":{\"name\":\"file_read\","
                 "\"arguments\":\"{\\\"path\\\":\\\"README\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target, "README");
  munit_assert_true(strstr(captured_logs, "[tool] call name=file_read") != NULL);
  munit_assert_true(strstr(captured_logs, "[permit] tool=file_read") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_chunked_tool_call_arguments_continue_with_tool_result(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "ai/providers.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_on_messages(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_chunked\",\"function\":{\"name\":\"fetch\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"function\":{\"arguments\":\"{\\\"url\\\"\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"function\":{\"arguments\":\":\\\"https://example.com\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "fetch");
  munit_assert_string_equal(captured_permit_target, "https://example.com");
  munit_assert_true(strstr(captured_logs, "[stream] tool_final") != NULL);
  munit_assert_true(strstr(captured_logs, "final_tool_calls=1") != NULL);
  munit_assert_true(strstr(captured_logs, "[tools] continuing with tool results") != NULL);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"tool_call_id\":\"call_chunked\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/request_enables_auto_tool_choice", test_request_enables_auto_tool_choice,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/fetch_permission_target_uses_url", test_fetch_permission_target_uses_url,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/logs_text_when_model_does_not_call_tool",
     test_logs_text_when_model_does_not_call_tool, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_permission_target_uses_path",
     test_file_read_permission_target_uses_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/chunked_tool_call_arguments_continue_with_tool_result",
     test_chunked_tool_call_arguments_continue_with_tool_result, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite provider_tools_suite = {"/provider_tools", tests, NULL, 1,
                                   MUNIT_SUITE_OPTION_NONE};
