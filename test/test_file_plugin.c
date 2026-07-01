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

static void load_file_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void set_capstan_workdir(lua_State *L, const char *path) {
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "workdir");
  lua_pushcfunction(L, l_capstan_realpath);
  lua_setfield(L, -2, "realpath");
  lua_setglobal(L, "capstan");
}

static int path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void call_handler(lua_State *L, const char *path) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_handler_tool(lua_State *L, const char *path) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  lua_setfield(L, -2, "args");
  lua_newtable(L);
  lua_pushstring(L, path);
  lua_setfield(L, -2, "path");
  lua_setfield(L, -2, "tool_args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_readme_fallback_reads_readme_md(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_state();
  load_file_plugin(L);

  call_handler(L, "README");

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, "README.md") != NULL);
  munit_assert_true(strstr(llm, "README.md") != NULL);
  munit_assert_true(strstr(llm, "# Capstan") != NULL);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_relative_path_uses_capstan_workdir(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char dir[4096];
  snprintf(dir, sizeof(dir), "/tmp/capstan-file-read-%ld", (long)getpid());
  rmdir(dir);
  munit_assert_int(mkdir(dir, 0700), ==, 0);

  char path[4096];
  snprintf(path, sizeof(path), "%s/NOTE.md", dir);
  FILE *f = fopen(path, "w");
  munit_assert_not_null(f);
  fputs("workspace note", f);
  fclose(f);

  lua_State *L = new_state();
  set_capstan_workdir(L, dir);
  load_file_plugin(L);

  call_handler(L, "NOTE.md");

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, path) != NULL);
  munit_assert_true(strstr(llm, path) != NULL);
  munit_assert_true(strstr(llm, "workspace note") != NULL);

  lua_close(L);
  unlink(path);
  rmdir(dir);
  return MUNIT_OK;
}

static MunitResult test_directory_listing_shell_quotes_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char dir[4096];
  snprintf(dir, sizeof(dir), "/tmp/capstan-file-injection-%ld",
           (long)getpid());
  rmdir(dir);
  munit_assert_int(mkdir(dir, 0700), ==, 0);

  char marker[4096];
  snprintf(marker, sizeof(marker), "%s/marker", dir);
  unlink(marker);

  char malicious[8192];
  snprintf(malicious, sizeof(malicious), "%s\"; touch %s; echo \"", dir,
           marker);

  lua_State *L = new_state();
  load_file_plugin(L);
  call_handler(L, malicious);

  munit_assert_false(path_exists(marker));

  lua_close(L);
  unlink(marker);
  rmdir(dir);
  return MUNIT_OK;
}

static MunitResult test_directory_path_returns_listing(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char dir[4096];
  snprintf(dir, sizeof(dir), "/tmp/capstan-file-dir-%ld", (long)getpid());
  rmdir(dir);
  munit_assert_int(mkdir(dir, 0700), ==, 0);

  char child[4096];
  snprintf(child, sizeof(child), "%s/child.txt", dir);
  FILE *f = fopen(child, "w");
  munit_assert_not_null(f);
  fputs("child", f);
  fclose(f);

  lua_State *L = new_state();
  load_file_plugin(L);
  call_handler(L, dir);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, "📁") != NULL);
  munit_assert_true(strstr(llm, "child.txt") != NULL);
  munit_assert_true(strstr(llm, "nil") == NULL);

  lua_close(L);
  unlink(child);
  rmdir(dir);
  return MUNIT_OK;
}

static MunitResult test_tool_read_rejects_symlink_escape(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  char outside[4096];
  snprintf(workdir, sizeof(workdir), "/tmp/capstan-file-link-work-%ld",
           (long)getpid());
  snprintf(outside, sizeof(outside), "/tmp/capstan-file-link-out-%ld",
           (long)getpid());
  rmdir(workdir);
  rmdir(outside);
  munit_assert_int(mkdir(workdir, 0700), ==, 0);
  munit_assert_int(mkdir(outside, 0700), ==, 0);

  char secret[4096];
  snprintf(secret, sizeof(secret), "%s/secret.txt", outside);
  FILE *f = fopen(secret, "w");
  munit_assert_not_null(f);
  fputs("outside secret", f);
  fclose(f);

  char link_path[4096];
  snprintf(link_path, sizeof(link_path), "%s/link.txt", workdir);
  munit_assert_int(symlink(secret, link_path), ==, 0);

  lua_State *L = new_state();
  set_capstan_workdir(L, workdir);
  load_file_plugin(L);
  call_handler_tool(L, "link.txt");

  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(llm);
  munit_assert_null(strstr(llm, "outside secret"));
  munit_assert_not_null(strstr(llm, "escapes workspace"));

  lua_close(L);
  unlink(link_path);
  unlink(secret);
  rmdir(outside);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/readme_fallback_reads_readme_md", test_readme_fallback_reads_readme_md,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/relative_path_uses_capstan_workdir",
     test_relative_path_uses_capstan_workdir, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/directory_listing_shell_quotes_path",
     test_directory_listing_shell_quotes_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/directory_path_returns_listing", test_directory_path_returns_listing,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_read_rejects_symlink_escape",
     test_tool_read_rejects_symlink_escape, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite file_plugin_suite = {"/file_plugin", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
