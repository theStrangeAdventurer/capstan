#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
  return L;
}

static void load_file_write_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file_write.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L, const char *path, const char *content) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, content);
  lua_rawseti(L, -2, 2);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void set_capstan_workdir(lua_State *L, const char *path) {
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "workdir");
  lua_setglobal(L, "capstan");
}

static void make_tmp_dir(char *buf, size_t buf_size, const char *name) {
  snprintf(buf, buf_size, "/tmp/capstan-%s-%ld", name, (long)getpid());
  rmdir(buf);
  munit_assert_int(mkdir(buf, 0700), ==, 0);
}

static MunitResult test_relative_path_uses_launch_pwd(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char dir[4096];
  make_tmp_dir(dir, sizeof(dir), "file-write-pwd");

  char old_pwd[4096];
  const char *prev_pwd = getenv("PWD");
  if (prev_pwd)
    snprintf(old_pwd, sizeof(old_pwd), "%s", prev_pwd);

  munit_assert_int(setenv("PWD", dir, 1), ==, 0);

  lua_State *L = new_state();
  load_file_write_plugin(L);
  call_handler(L, "SEC_CHECK.md", "report body");

  char expected[4096];
  snprintf(expected, sizeof(expected), "%s/SEC_CHECK.md", dir);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, expected) != NULL);
  munit_assert_true(strstr(llm, expected) != NULL);

  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "report body");

  lua_close(L);
  unlink(expected);
  rmdir(dir);

  if (prev_pwd)
    munit_assert_int(setenv("PWD", old_pwd, 1), ==, 0);
  else
    munit_assert_int(unsetenv("PWD"), ==, 0);

  return MUNIT_OK;
}

static MunitResult test_relative_path_prefers_capstan_workdir(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char pwd_dir[4096];
  char workdir[4096];
  make_tmp_dir(pwd_dir, sizeof(pwd_dir), "file-write-pwd");
  make_tmp_dir(workdir, sizeof(workdir), "file-write-workdir");

  char old_pwd[4096];
  const char *prev_pwd = getenv("PWD");
  if (prev_pwd)
    snprintf(old_pwd, sizeof(old_pwd), "%s", prev_pwd);

  munit_assert_int(setenv("PWD", pwd_dir, 1), ==, 0);

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler(L, "RESUME.md", "resume body");

  char expected[4096];
  char wrong[4096];
  snprintf(expected, sizeof(expected), "%s/RESUME.md", workdir);
  snprintf(wrong, sizeof(wrong), "%s/RESUME.md", pwd_dir);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, expected) != NULL);
  munit_assert_true(strstr(llm, expected) != NULL);

  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  fclose(f);
  munit_assert_null(fopen(wrong, "r"));

  lua_close(L);
  unlink(expected);
  rmdir(workdir);
  rmdir(pwd_dir);

  if (prev_pwd)
    munit_assert_int(setenv("PWD", old_pwd, 1), ==, 0);
  else
    munit_assert_int(unsetenv("PWD"), ==, 0);

  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/relative_path_uses_launch_pwd", test_relative_path_uses_launch_pwd, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/relative_path_prefers_capstan_workdir",
     test_relative_path_prefers_capstan_workdir, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite file_write_plugin_suite = {"/file_write_plugin", tests, NULL, 1,
                                      MUNIT_SUITE_OPTION_NONE};
