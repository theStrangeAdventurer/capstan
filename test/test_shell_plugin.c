#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

static char captured_shell_command[512];
static int captured_shell_timeout = 0;

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static int l_tools_shell(lua_State *L) {
  const char *command = luaL_checkstring(L, 1);
  int timeout = (int)luaL_optinteger(L, 2, 0);
  snprintf(captured_shell_command, sizeof(captured_shell_command), "%s",
           command);
  captured_shell_timeout = timeout;

  lua_newtable(L);
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "exit");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "timed_out");
  lua_pushstring(L,
                 "HTTP/1.1 200 OK\n"
                 "Authorization: Bearer stdout-secret\n"
                 "X-API-Key: stdout-api-key\n"
                 "X-Subscription-Key: brave-secret\n"
                 "x-subscription-token: subscription-secret\n"
                 "xsubscription.token: dotted-secret\n"
                 "Tenant-Id: tenant-plain\n"
                 "X-Internal-Trace: internal-plain\n"
                 "> Accept: */*\n"
                 "custom value org_custom-secret\n"
                 "{\"access_token\":\"json-secret\"}\n");
  lua_setfield(L, -2, "stdout");
  lua_pushstring(L,
                 "Cookie: session=stderr-secret\n"
                 "password=stderr-password\n");
  lua_setfield(L, -2, "stderr");
  return 1;
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_pushcfunction(L, l_tools_shell);
  lua_setfield(L, -2, "shell");
  lua_setglobal(L, "tools");

  return L;
}

static void load_shell_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/shell.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L, const char *input, const char *args[],
                         int arg_count) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_pushstring(L, input);
  lua_setfield(L, -2, "input");
  lua_pushstring(L, "/shell");
  lua_setfield(L, -2, "command");
  lua_newtable(L);
  for (int i = 0; i < arg_count; i++) {
    lua_pushstring(L, args[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_shell_redacts_ui_and_llm_results(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  load_shell_plugin(L);
  const char *args[] = {"curl", "-v", "-H", "Authorization: Bearer command-secret",
                        "https://example.test"};
  call_handler(L,
               "/shell curl -v -H 'Authorization: Bearer command-secret' https://example.test",
               args, 5);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);

  munit_assert_true(strstr(ui, "command-secret") == NULL);
  munit_assert_true(strstr(ui, "Shell: curl https://example.test") != NULL);
  munit_assert_true(strstr(ui, "Authorization") == NULL);

  munit_assert_true(strstr(llm, "stdout-secret") == NULL);
  munit_assert_true(strstr(llm, "stdout-api-key") == NULL);
  munit_assert_true(strstr(llm, "brave-secret") == NULL);
  munit_assert_true(strstr(llm, "subscription-secret") == NULL);
  munit_assert_true(strstr(llm, "dotted-secret") == NULL);
  munit_assert_true(strstr(llm, "json-secret") == NULL);
  munit_assert_true(strstr(llm, "stderr-secret") == NULL);
  munit_assert_true(strstr(llm, "stderr-password") == NULL);
  munit_assert_true(strstr(llm, "Authorization: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "X-API-Key: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "X-Subscription-Key: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "x-subscription-token: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "xsubscription.token: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "> Accept: */*") != NULL);
  munit_assert_true(strstr(llm, "\"access_token\":\"[REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "Cookie: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "password=[REDACTED]") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_manual_command_preserves_spaces(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  captured_shell_command[0] = '\0';
  captured_shell_timeout = 0;
  lua_State *L = new_state();
  load_shell_plugin(L);

  const char *args[] = {"ls", "-la", "/tmp/capstan test"};
  call_handler(L, "/shell --timeout 7 ls -la \"/tmp/capstan test\"", args, 3);

  munit_assert_string_equal(captured_shell_command,
                            "ls -la \"/tmp/capstan test\"");
  munit_assert_int(captured_shell_timeout, ==, 7);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_redaction_uses_config_extensions(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  int rc = luaL_dostring(L,
                         "capstan = { config = { redaction = {"
                         "names = {'tenant-id'},"
                         "name_patterns = {'^x%-internal%-'},"
                         "value_patterns = {'org_[%w%-]+'},"
                         "} } }");
  munit_assert_int(rc, ==, LUA_OK);

  load_shell_plugin(L);
  const char *args[] = {"curl", "-s", "https://example.test"};
  call_handler(L, "/shell curl -s https://example.test", args, 3);

  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(llm, "tenant-plain") == NULL);
  munit_assert_true(strstr(llm, "internal-plain") == NULL);
  munit_assert_true(strstr(llm, "org_custom-secret") == NULL);
  munit_assert_true(strstr(llm, "Tenant-Id: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "X-Internal-Trace: [REDACTED]") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/redacts_ui_and_llm_results", test_shell_redacts_ui_and_llm_results, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/manual_command_preserves_spaces",
     test_shell_manual_command_preserves_spaces, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/redaction_uses_config_extensions",
     test_shell_redaction_uses_config_extensions, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite shell_plugin_suite = {"/shell_plugin", tests, NULL, 1,
                                 MUNIT_SUITE_OPTION_NONE};
