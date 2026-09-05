#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

static char captured_shell_command[512];
static int captured_shell_timeout = 0;
static int simulated_shell_exit = 0;
static int simulated_shell_timeout = 0;
static const char *stdout_override = NULL;
static const char *stderr_override = NULL;

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
  lua_pushinteger(L, simulated_shell_exit);
  lua_setfield(L, -2, "exit");
  lua_pushboolean(L, simulated_shell_timeout);
  lua_setfield(L, -2, "timed_out");
  lua_pushstring(L, stdout_override ? stdout_override :
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
  lua_pushstring(L, stderr_override ? stderr_override :
                 "Cookie: session=stderr-secret\n"
                 "password=stderr-password\n");
  lua_setfield(L, -2, "stderr");
  return 1;
}

static lua_State *new_state(void) {
  simulated_shell_exit = 0;
  simulated_shell_timeout = 0;
  stdout_override = NULL;
  stderr_override = NULL;
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  munit_assert_int(luaL_dostring(L,
      "capstan = {log = function(category, message) shell_log = message end}"),
      ==, LUA_OK);

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

  int rc = lua_pcall(L, 1, 3, 0);
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

  const char *ui = lua_tostring(L, -3);
  const char *llm = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);

  munit_assert_true(strstr(ui, "command-secret") == NULL);
  munit_assert_true(strstr(ui, "Shell: curl https://example.test") != NULL);
  munit_assert_true(strstr(ui, "Authorization: [REDACTED]") != NULL);
  munit_assert_string_equal(strchr(ui, '\n') + 1, llm);
  lua_getglobal(L, "shell_log");
  const char *logged = lua_tostring(L, -1);
  munit_assert_not_null(logged);
  munit_assert_true(strstr(logged, "shell result command=curl https://example.test") != NULL);
  munit_assert_string_equal(strchr(logged, '\n') + 1, llm);
  lua_pop(L, 1);

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
  munit_assert_true(lua_toboolean(L, -1));

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

  const char *llm = lua_tostring(L, -2);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(llm, "tenant-plain") == NULL);
  munit_assert_true(strstr(llm, "internal-plain") == NULL);
  munit_assert_true(strstr(llm, "org_custom-secret") == NULL);
  munit_assert_true(strstr(llm, "Tenant-Id: [REDACTED]") != NULL);
  munit_assert_true(strstr(llm, "X-Internal-Trace: [REDACTED]") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_nonzero_exit_reports_failure(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  simulated_shell_exit = 1;
  load_shell_plugin(L);
  const char *args[] = {"npm", "test"};
  call_handler(L, "/shell npm test", args, 2);

  munit_assert_true(strstr(lua_tostring(L, -3), "exit 1") != NULL);
  munit_assert_true(strstr(lua_tostring(L, -2), "[exit 1]") != NULL);
  munit_assert_false(lua_toboolean(L, -1));
  const char *ui = lua_tostring(L, -3);
  munit_assert_not_null(strstr(ui, "HTTP/1.1 200 OK"));
  munit_assert_not_null(strstr(ui, "stderr:\n"));
  lua_getglobal(L, "shell_log");
  munit_assert_string_equal(strchr(lua_tostring(L, -1), '\n') + 1,
                            strchr(ui, '\n') + 1);

  simulated_shell_exit = 0;
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_output_empty_timeout_and_limits(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  stdout_override = "";
  stderr_override = "";
  load_shell_plugin(L);
  const char *args[] = {"git", "status"};
  call_handler(L, "/shell git status", args, 2);
  munit_assert_string_equal(lua_tostring(L, -3),
                            "Shell: git status\n[exit 0]");
  lua_pop(L, 3);

  simulated_shell_timeout = 1;
  stdout_override = "partial output\n";
  stderr_override = "timeout diagnostic\n";
  call_handler(L, "/shell -t 7 git status", args, 2);
  munit_assert_not_null(strstr(lua_tostring(L, -3), "TIMED OUT after 7s"));
  munit_assert_not_null(strstr(lua_tostring(L, -3), "partial output"));
  munit_assert_not_null(strstr(lua_tostring(L, -3), "stderr:\ntimeout diagnostic"));
  munit_assert_false(lua_toboolean(L, -1));
  lua_getglobal(L, "shell_log");
  munit_assert_not_null(strstr(lua_tostring(L, -1), "TIMED OUT after 7s"));
  lua_pop(L, 4);

  simulated_shell_timeout = 0;
  stdout_override = "visible\nhidden\n";
  stderr_override = "";
  munit_assert_int(luaL_dostring(L,
      "capstan.config = {tool_output = {max_lines = 2}}"), ==, LUA_OK);
  call_handler(L, "/shell git status", args, 2);
  const char *ui = lua_tostring(L, -3);
  munit_assert_not_null(strstr(ui, "visible"));
  munit_assert_null(strstr(ui, "hidden"));
  munit_assert_not_null(strstr(ui, "[Tool output truncated:"));
  lua_getglobal(L, "shell_log");
  munit_assert_string_equal(strchr(lua_tostring(L, -1), '\n') + 1,
                            strchr(ui, '\n') + 1);
  lua_pop(L, 4);

  char large_output[4096];
  memset(large_output, 'x', sizeof(large_output) - 1);
  large_output[sizeof(large_output) - 1] = '\0';
  stdout_override = large_output;
  munit_assert_int(luaL_dostring(L,
      "capstan.config.tool_output = {max_bytes = 1024}"), ==, LUA_OK);
  call_handler(L, "/shell git status", args, 2);
  ui = lua_tostring(L, -3);
  munit_assert_size(strlen(strchr(ui, '\n') + 1), <=, 1024);
  munit_assert_not_null(strstr(ui, "[Tool output truncated:"));
  lua_getglobal(L, "shell_log");
  munit_assert_string_equal(strchr(lua_tostring(L, -1), '\n') + 1,
                            strchr(ui, '\n') + 1);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_multiline_metadata(
    const MunitParameter params[], void *data) {
  (void)params; (void)data;
  lua_State *L = new_state();
  munit_assert_int(luaL_dostring(L,
      "local plugin = dofile('plugins/shell.lua')\n"
      "local command = \"cat <<'END'\\n[exit 99]\\nEND\\nfalse\"\n"
      "local ui, raw, ok, metadata = plugin.handler({\n"
      "  input = '/shell ' .. command, command = '/shell'})\n"
      "local header = 'Shell: ' .. command .. '\\n'\n"
      "assert(metadata.shell_output_start == #header)\n"
      "assert(ui:sub(metadata.shell_output_start + 1) == raw)\n"
      "assert(ui:sub(1, #header) == header)\n"), ==, LUA_OK);
  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/multiline_metadata", test_shell_multiline_metadata, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/output_empty_timeout_and_limits", test_shell_output_empty_timeout_and_limits,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/redacts_ui_and_llm_results", test_shell_redacts_ui_and_llm_results, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/manual_command_preserves_spaces",
     test_shell_manual_command_preserves_spaces, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/redaction_uses_config_extensions",
     test_shell_redaction_uses_config_extensions, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/nonzero_exit_reports_failure", test_shell_nonzero_exit_reports_failure,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite shell_plugin_suite = {"/shell_plugin", tests, NULL, 1,
                                 MUNIT_SUITE_OPTION_NONE};
