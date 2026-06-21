#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
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

static void set_capstan_workdir(lua_State *L, const char *path) {
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "workdir");
  lua_setglobal(L, "capstan");
}

static void load_file_edit_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file_edit.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void make_tmp_dir(char *buf, size_t buf_size, const char *name) {
  snprintf(buf, buf_size, "/tmp/capstan-%s-%ld", name, (long)getpid());
  rmdir(buf);
  munit_assert_int(mkdir(buf, 0700), ==, 0);
}

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "wb");
  munit_assert_not_null(f);
  munit_assert_size(fwrite(content, 1, strlen(content), f), ==,
                    strlen(content));
  fclose(f);
}

static void read_file(const char *path, char *buf, size_t buf_size) {
  FILE *f = fopen(path, "rb");
  munit_assert_not_null(f);
  size_t n = fread(buf, 1, buf_size - 1, f);
  fclose(f);
  buf[n] = '\0';
}

static void call_handler_tool(lua_State *L, const char *path,
                              const char *old_text, const char *new_text,
                              int replace_all) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_setfield(L, -2, "args");

  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "path");
  lua_pushstring(L, old_text);
  lua_setfield(L, -2, "old_text");
  lua_pushstring(L, new_text);
  lua_setfield(L, -2, "new_text");
  lua_pushboolean(L, replace_all);
  lua_setfield(L, -2, "replace_all");
  lua_setfield(L, -2, "tool_args");

  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_replaces_single_exact_fragment(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-edit-single");
  char path[4096];
  snprintf(path, sizeof(path), "%s/file.txt", workdir);
  write_file(path, "alpha\nbeta\ngamma\n");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_edit_plugin(L);
  call_handler_tool(L, "file.txt", "beta", "BETA", 0);

  char buf[128];
  read_file(path, buf, sizeof(buf));
  munit_assert_string_equal(buf, "alpha\nBETA\ngamma\n");
  munit_assert_true(strstr(lua_tostring(L, -2), "1 replacement") != NULL);

  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_not_found_does_not_write(const MunitParameter params[],
                                                 void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-edit-not-found");
  char path[4096];
  snprintf(path, sizeof(path), "%s/file.txt", workdir);
  write_file(path, "alpha\n");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_edit_plugin(L);
  call_handler_tool(L, "file.txt", "missing", "new", 0);

  char buf[128];
  read_file(path, buf, sizeof(buf));
  munit_assert_string_equal(buf, "alpha\n");
  munit_assert_true(strstr(lua_tostring(L, -2), "old_text not found") != NULL);

  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_ambiguous_match_does_not_write(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-edit-ambiguous");
  char path[4096];
  snprintf(path, sizeof(path), "%s/file.txt", workdir);
  write_file(path, "dup\ndup\n");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_edit_plugin(L);
  call_handler_tool(L, "file.txt", "dup", "new", 0);

  char buf[128];
  read_file(path, buf, sizeof(buf));
  munit_assert_string_equal(buf, "dup\ndup\n");
  munit_assert_true(strstr(lua_tostring(L, -2), "matched 2 times") != NULL);

  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_replace_all(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-edit-all");
  char path[4096];
  snprintf(path, sizeof(path), "%s/file.txt", workdir);
  write_file(path, "dup\ndup\n");

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_edit_plugin(L);
  call_handler_tool(L, "file.txt", "dup", "new", 1);

  char buf[128];
  read_file(path, buf, sizeof(buf));
  munit_assert_string_equal(buf, "new\nnew\n");
  munit_assert_true(strstr(lua_tostring(L, -2), "2 replacements") != NULL);

  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_preserves_utf8_bom(const MunitParameter params[],
                                           void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "file-edit-bom");
  char path[4096];
  snprintf(path, sizeof(path), "%s/file.txt", workdir);
  FILE *f = fopen(path, "wb");
  munit_assert_not_null(f);
  unsigned char bom[] = {0xef, 0xbb, 0xbf};
  munit_assert_size(fwrite(bom, 1, sizeof(bom), f), ==, sizeof(bom));
  munit_assert_size(fwrite("old", 1, 3, f), ==, 3);
  fclose(f);

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_edit_plugin(L);
  call_handler_tool(L, "file.txt", "old", "new", 0);

  unsigned char buf[16];
  f = fopen(path, "rb");
  munit_assert_not_null(f);
  size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  munit_assert_size(n, ==, 6);
  munit_assert_uchar(buf[0], ==, 0xef);
  munit_assert_uchar(buf[1], ==, 0xbb);
  munit_assert_uchar(buf[2], ==, 0xbf);
  munit_assert_memory_equal(3, buf + 3, "new");

  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/replaces_single_exact_fragment", test_replaces_single_exact_fragment,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/not_found_does_not_write", test_not_found_does_not_write, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/ambiguous_match_does_not_write", test_ambiguous_match_does_not_write,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/replace_all", test_replace_all, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/preserves_utf8_bom", test_preserves_utf8_bom, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite file_edit_plugin_suite = {"/file_edit_plugin", tests, NULL, 1,
                                     MUNIT_SUITE_OPTION_NONE};
