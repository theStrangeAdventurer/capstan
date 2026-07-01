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

static int l_capstan_realpath(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  char resolved[4096];
  if (!realpath(path, resolved)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, resolved);
  return 1;
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

static void call_handler_tool(lua_State *L, const char *path, const char *content) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_setfield(L, -2, "args");

  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "path");
  lua_pushstring(L, content);
  lua_setfield(L, -2, "content");
  lua_setfield(L, -2, "tool_args");

  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_handler_tool_mode(lua_State *L, const char *path,
                                   const char *content, const char *mode) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_setfield(L, -2, "args");

  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "path");
  lua_pushstring(L, content);
  lua_setfield(L, -2, "content");
  lua_pushstring(L, mode);
  lua_setfield(L, -2, "mode");
  lua_setfield(L, -2, "tool_args");

  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_handler_tool_with_positional_noise(lua_State *L,
                                                    const char *path,
                                                    const char *content) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "wrong/path.txt");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "wrong content");
  lua_rawseti(L, -2, 2);
  lua_setfield(L, -2, "args");

  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "path");
  lua_pushstring(L, content);
  lua_setfield(L, -2, "content");
  lua_setfield(L, -2, "tool_args");

  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_handler_append(lua_State *L, const char *path,
                                const char *content) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "--append");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, path);
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, content);
  lua_rawseti(L, -2, 3);
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
  lua_pushcfunction(L, l_capstan_realpath);
  lua_setfield(L, -2, "realpath");
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

static MunitResult test_creates_parent_directories(const MunitParameter params[],
                                                  void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-nested");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler(L, "nested/deep/file.txt", "nested body");

  char expected[4096];
  snprintf(expected, sizeof(expected), "%s/nested/deep/file.txt", workdir);

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Created ") != NULL);
  munit_assert_true(strstr(ui, expected) != NULL);

  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "nested body");

  lua_close(L);
  unlink(expected);
  char nested_deep[4096];
  char nested[4096];
  snprintf(nested_deep, sizeof(nested_deep), "%s/nested/deep", workdir);
  snprintf(nested, sizeof(nested), "%s/nested", workdir);
  rmdir(nested_deep);
  rmdir(nested);
  rmdir(workdir);

  return MUNIT_OK;
}

static MunitResult test_preserves_existing_utf8_bom(const MunitParameter params[],
                                                    void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-bom");

  char path[4096];
  snprintf(path, sizeof(path), "%s/existing.txt", workdir);
  FILE *seed = fopen(path, "wb");
  munit_assert_not_null(seed);
  const unsigned char bom[] = {0xef, 0xbb, 0xbf};
  munit_assert_size(fwrite(bom, 1, sizeof(bom), seed), ==, sizeof(bom));
  munit_assert_size(fwrite("old", 1, 3, seed), ==, 3);
  fclose(seed);

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler(L, "existing.txt", "new");

  FILE *f = fopen(path, "rb");
  munit_assert_not_null(f);
  unsigned char buf[16];
  size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  munit_assert_size(n, ==, 6);
  munit_assert_uchar(buf[0], ==, 0xef);
  munit_assert_uchar(buf[1], ==, 0xbb);
  munit_assert_uchar(buf[2], ==, 0xbf);
  munit_assert_memory_equal(3, buf + 3, "new");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Wrote ") != NULL);
  munit_assert_true(strstr(ui, "UTF-8 BOM") != NULL);

  lua_close(L);
  unlink(path);
  rmdir(workdir);

  return MUNIT_OK;
}

static MunitResult test_tool_args_preserve_multiline_content(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-tool");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler_tool(L, "tool.txt", "one two\nthree");

  char expected[4096];
  snprintf(expected, sizeof(expected), "%s/tool.txt", workdir);
  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "one two\nthree");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "13 bytes, 2 lines") != NULL);

  lua_close(L);
  unlink(expected);
  rmdir(workdir);

  return MUNIT_OK;
}

static MunitResult test_tool_args_take_precedence_over_positional_args(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-tool-precedence");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler_tool_with_positional_noise(L, "tool-real.txt", "real content");

  char expected[4096];
  char wrong[4096];
  snprintf(expected, sizeof(expected), "%s/tool-real.txt", workdir);
  snprintf(wrong, sizeof(wrong), "%s/wrong/path.txt", workdir);
  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "real content");
  munit_assert_null(fopen(wrong, "r"));

  lua_close(L);
  unlink(expected);
  rmdir(workdir);

  return MUNIT_OK;
}

static MunitResult test_append_mode_preserves_existing_content(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-append");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler(L, "notes.txt", "one\n");
  lua_pop(L, 2);
  call_handler_append(L, "notes.txt", "two\n");

  char expected[4096];
  snprintf(expected, sizeof(expected), "%s/notes.txt", workdir);
  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "one\ntwo\n");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Appended to ") != NULL);

  lua_close(L);
  unlink(expected);
  rmdir(workdir);

  return MUNIT_OK;
}

static MunitResult test_tool_args_append_mode_preserves_existing_content(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-tool-append");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler_tool(L, "tool-append.txt", "alpha");
  lua_pop(L, 2);
  call_handler_tool_mode(L, "tool-append.txt", "\nbeta", "append");

  char expected[4096];
  snprintf(expected, sizeof(expected), "%s/tool-append.txt", workdir);
  FILE *f = fopen(expected, "r");
  munit_assert_not_null(f);
  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "alpha\nbeta");

  lua_close(L);
  unlink(expected);
  rmdir(workdir);

  return MUNIT_OK;
}

static MunitResult test_tool_metadata_exposes_append_mode(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  load_file_write_plugin(L);

  lua_getfield(L, -1, "tool");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "parameters");
  lua_getfield(L, -1, "properties");
  lua_getfield(L, -1, "mode");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "enum");
  munit_assert_true(lua_istable(L, -1));
  lua_rawgeti(L, -1, 2);
  munit_assert_string_equal(lua_tostring(L, -1), "append");

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_write_rejects_symlink_escape(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  char outside[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-write-link-work");
  make_tmp_dir(outside, sizeof(outside), "file-write-link-out");

  char secret[4096];
  snprintf(secret, sizeof(secret), "%s/secret.txt", outside);
  FILE *f = fopen(secret, "w");
  munit_assert_not_null(f);
  fputs("keep me", f);
  fclose(f);

  char link_path[4096];
  snprintf(link_path, sizeof(link_path), "%s/link.txt", workdir);
  munit_assert_int(symlink(secret, link_path), ==, 0);

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_write_plugin(L);
  call_handler_tool(L, "link.txt", "overwrite");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_not_null(strstr(ui, "escapes workspace"));

  char buf[64];
  f = fopen(secret, "r");
  munit_assert_not_null(f);
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  munit_assert_string_equal(buf, "keep me");

  lua_close(L);
  unlink(link_path);
  unlink(secret);
  rmdir(outside);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/relative_path_uses_launch_pwd", test_relative_path_uses_launch_pwd, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/relative_path_prefers_capstan_workdir",
     test_relative_path_prefers_capstan_workdir, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/creates_parent_directories", test_creates_parent_directories, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/preserves_existing_utf8_bom", test_preserves_existing_utf8_bom, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_args_preserve_multiline_content",
     test_tool_args_preserve_multiline_content, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_args_take_precedence_over_positional_args",
     test_tool_args_take_precedence_over_positional_args, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/append_mode_preserves_existing_content",
     test_append_mode_preserves_existing_content, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_args_append_mode_preserves_existing_content",
     test_tool_args_append_mode_preserves_existing_content, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_metadata_exposes_append_mode",
     test_tool_metadata_exposes_append_mode, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_write_rejects_symlink_escape",
     test_tool_write_rejects_symlink_escape, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite file_write_plugin_suite = {"/file_write_plugin", tests, NULL, 1,
                                      MUNIT_SUITE_OPTION_NONE};
