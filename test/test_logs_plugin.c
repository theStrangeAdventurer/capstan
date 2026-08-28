#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char temp_log_path[512];
static char temp_locked_log_path[512];

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static int l_log_path(lua_State *L) {
  lua_pushstring(L, temp_log_path);
  return 1;
}

static int l_log_read_lock(lua_State *L) {
  lua_pushboolean(L, 1);
  if (temp_locked_log_path[0])
    lua_pushstring(L, temp_locked_log_path);
  else
    lua_pushnil(L);
  return 2;
}

static int l_log_read_unlock(lua_State *L) {
  (void)L;
  return 0;
}

static int l_log_read_tail(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  lua_Integer requested = luaL_checkinteger(L, 2);
  FILE *file = fopen(path, "rb");
  if (!file) {
    lua_pushnil(L);
    lua_pushinteger(L, 0);
    return 2;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  long amount = size < requested ? size : (long)requested;
  long start = size - amount;
  fseek(file, start, SEEK_SET);
  char *buffer = malloc((size_t)amount + 1);
  munit_assert_not_null(buffer);
  size_t read_size = fread(buffer, 1, (size_t)amount, file);
  fclose(file);
  lua_pushlstring(L, buffer, read_size);
  lua_pushinteger(L, start);
  free(buffer);
  return 2;
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  lua_newtable(L);
  lua_pushcfunction(L, l_log_path);
  lua_setfield(L, -2, "log_path");
  lua_pushcfunction(L, l_log_read_lock);
  lua_setfield(L, -2, "log_read_lock");
  lua_pushcfunction(L, l_log_read_unlock);
  lua_setfield(L, -2, "log_read_unlock");
  lua_pushcfunction(L, l_log_read_tail);
  lua_setfield(L, -2, "log_read_tail");
  lua_setglobal(L, "capstan");

  return L;
}

static void write_temp_log(void) {
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-log-%ld.txt", (long)getpid());
  FILE *f = fopen(temp_log_path, "w");
  munit_assert_not_null(f);
  fputs("one\n", f);
  fputs("two\n", f);
  fputs("three\n", f);
  fclose(f);
}

static void load_logs_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/logs.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L, const char *limit) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  if (limit) {
    lua_pushstring(L, limit);
    lua_rawseti(L, -2, 1);
  }
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_logs_tail_limit(const MunitParameter params[],
                                        void *data) {
  (void)params;
  (void)data;
  write_temp_log();
  lua_State *L = new_state();
  load_logs_plugin(L);

  call_handler(L, "2");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, temp_log_path) != NULL);
  munit_assert_true(strstr(ui, "one") == NULL);
  munit_assert_true(strstr(ui, "two") != NULL);
  munit_assert_true(strstr(ui, "three") != NULL);

  lua_close(L);
  unlink(temp_log_path);
  return MUNIT_OK;
}

static MunitResult test_logs_uses_path_returned_by_read_lock(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-log-old-day-%ld.jsonl", (long)getpid());
  snprintf(temp_locked_log_path, sizeof(temp_locked_log_path),
           "/tmp/capstan-test-log-locked-day-%ld.jsonl", (long)getpid());
  unlink(temp_log_path);
  FILE *f = fopen(temp_locked_log_path, "w");
  munit_assert_not_null(f);
  fputs("locked-day-marker\n", f);
  fclose(f);

  lua_State *L = new_state();
  load_logs_plugin(L);
  call_handler(L, "10");
  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_not_null(strstr(ui, temp_locked_log_path));
  munit_assert_not_null(strstr(ui, "locked-day-marker"));
  munit_assert_null(strstr(ui, temp_log_path));

  lua_close(L);
  unlink(temp_locked_log_path);
  temp_locked_log_path[0] = '\0';
  return MUNIT_OK;
}

static MunitResult test_logs_flattens_structured_message_newlines(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-log-%ld.jsonl", (long)getpid());
  FILE *f = fopen(temp_log_path, "w");
  munit_assert_not_null(f);
  fputs("{\"schema\":\"capstan.log.v1\",\"timestamp\":\"now\","
        "\"level\":\"info\",\"category\":\"test\","
        "\"message\":\"first\\n[error] forged\"}\n", f);
  fclose(f);

  lua_State *L = new_state();
  load_logs_plugin(L);
  call_handler(L, "10");
  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_not_null(strstr(ui, "first [error] forged"));
  munit_assert_null(strstr(ui, "first\n[error] forged"));
  lua_close(L);
  unlink(temp_log_path);
  return MUNIT_OK;
}

static MunitResult test_logs_formats_long_structured_records_before_bounding(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-long-log-%ld.jsonl", (long)getpid());
  FILE *f = fopen(temp_log_path, "w");
  munit_assert_not_null(f);
  fputs("{\"schema\":\"capstan.log.v1\",\"timestamp\":\"now\","
        "\"level\":\"info\",\"category\":\"test\",\"message\":\"", f);
  for (int i = 0; i < 20000; i++)
    fputc('a', f);
  fputs("\"}\n", f);
  fclose(f);

  lua_State *L = new_state();
  load_logs_plugin(L);
  call_handler(L, "10");
  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_not_null(strstr(ui, "now [info] [test] aaaa"));
  munit_assert_null(strstr(ui, "{\"schema\""));
  munit_assert_not_null(strstr(ui, "<log record truncated;"));

  lua_close(L);
  unlink(temp_log_path);
  return MUNIT_OK;
}

static MunitResult test_logs_reads_rotated_legacy_archives(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-legacy-%ld.jsonl", (long)getpid());
  char archive[512];
  snprintf(archive, sizeof(archive), "/tmp/capstan-test-legacy-%ld.1.log",
           (long)getpid());
  unlink(temp_log_path);
  FILE *file = fopen(archive, "w");
  munit_assert_not_null(file);
  fputs("legacy archive marker\n", file);
  fclose(file);

  lua_State *L = new_state();
  load_logs_plugin(L);
  call_handler(L, "10");
  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_not_null(strstr(ui, "legacy archive marker"));
  lua_close(L);
  unlink(archive);
  return MUNIT_OK;
}

static MunitResult test_logs_sanitizes_control_characters_in_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  snprintf(temp_log_path, sizeof(temp_log_path),
           "/tmp/capstan-test-path-%ld-\033[31m.jsonl", (long)getpid());
  unlink(temp_log_path);
  lua_State *L = new_state();
  load_logs_plugin(L);
  call_handler(L, "10");
  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_null(strchr(ui, '\033'));
  munit_assert_not_null(strstr(ui, "\\u001B[31m"));
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_logs_tool_metadata(const MunitParameter params[],
                                           void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_logs_plugin(L);

  lua_getfield(L, -1, "tool");
  munit_assert_true(lua_istable(L, -1));

  lua_getfield(L, -1, "name");
  munit_assert_string_equal(lua_tostring(L, -1), "logs");
  lua_pop(L, 1);

  lua_getfield(L, -1, "description");
  munit_assert_true(strstr(lua_tostring(L, -1), "debug failed tools") != NULL);
  lua_pop(L, 1);

  lua_getfield(L, -1, "parameters");
  munit_assert_true(lua_istable(L, -1));
  lua_pop(L, 2);

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/tail_limit", test_logs_tail_limit, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/uses_path_returned_by_read_lock",
     test_logs_uses_path_returned_by_read_lock, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/flattens_structured_message_newlines",
     test_logs_flattens_structured_message_newlines, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/formats_long_structured_records_before_bounding",
     test_logs_formats_long_structured_records_before_bounding, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/reads_rotated_legacy_archives",
     test_logs_reads_rotated_legacy_archives, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/sanitizes_control_characters_in_path",
     test_logs_sanitizes_control_characters_in_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_metadata", test_logs_tool_metadata, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite logs_plugin_suite = {"/logs_plugin", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
