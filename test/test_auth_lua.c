#include "munit.h"
#include <fcntl.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char temp_dir[512];

static int l_state_path(lua_State *L) {
  const char *relative = luaL_optstring(L, 1, "state.lua");
  char path[768];
  snprintf(path, sizeof(path), "%s/%s", temp_dir, relative);
  lua_pushstring(L, path);
  return 1;
}

static int l_state_ensure_dir(lua_State *L) {
  lua_pushboolean(L, 1);
  return 1;
}

static int write_all_fd(int fd, const char *data, size_t size) {
  size_t written = 0;
  while (written < size) {
    ssize_t n = write(fd, data + written, size - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    written += (size_t)n;
  }
  return 0;
}

static int l_secure_write_file(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  size_t content_size = 0;
  const char *content = luaL_checklstring(L, 2, &content_size);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  if (fchmod(fd, 0600) != 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, strerror(errno));
    close(fd);
    return 2;
  }
  if (write_all_fd(fd, content, content_size) != 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, strerror(errno));
    close(fd);
    return 2;
  }
  if (close(fd) != 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static lua_State *new_auth_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  lua_newtable(L);
  lua_pushcfunction(L, l_state_path);
  lua_setfield(L, -2, "state_path");
  lua_pushcfunction(L, l_state_ensure_dir);
  lua_setfield(L, -2, "state_ensure_dir");
  lua_pushcfunction(L, l_secure_write_file);
  lua_setfield(L, -2, "secure_write_file");
  lua_setglobal(L, "capstan");
  return L;
}

static MunitResult test_auth_set_get_redact_remove(const MunitParameter params[],
                                                   void *data) {
  (void)params;
  (void)data;
  snprintf(temp_dir, sizeof(temp_dir), "/tmp/capstan-auth-test-%ld",
           (long)getpid());
  mkdir(temp_dir, 0700);

  lua_State *L = new_auth_state();
  int rc = luaL_dofile(L, "agent/auth.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "auth");

  lua_getfield(L, -1, "set");
  lua_pushstring(L, "oauth_test_provider");
  lua_newtable(L);
  lua_pushstring(L, "oauth");
  lua_setfield(L, -2, "type");
  lua_pushstring(L, "access-token-value");
  lua_setfield(L, -2, "access");
  lua_pushstring(L, "refresh-token-value");
  lua_setfield(L, -2, "refresh");
  lua_pushinteger(L, 12345);
  lua_setfield(L, -2, "expires");
  rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));
  lua_pop(L, 2);

  char auth_file[768];
  snprintf(auth_file, sizeof(auth_file), "%s/state/auth.lua", temp_dir);
  struct stat st;
  munit_assert_int(stat(auth_file, &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0600);

  lua_getfield(L, -1, "get");
  lua_pushstring(L, "oauth_test_provider");
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_getfield(L, -1, "refresh");
  munit_assert_string_equal(lua_tostring(L, -1), "refresh-token-value");
  lua_pop(L, 2);

  lua_getfield(L, -1, "redacted");
  lua_pushstring(L, "oauth_test_provider");
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_getfield(L, -1, "refresh");
  munit_assert_string_not_equal(lua_tostring(L, -1), "refresh-token-value");
  lua_pop(L, 2);

  lua_getfield(L, -1, "remove");
  lua_pushstring(L, "oauth_test_provider");
  rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));
  lua_pop(L, 2);

  lua_getfield(L, -1, "get");
  lua_pushstring(L, "oauth_test_provider");
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_isnil(L, -1));

  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/set_get_redact_remove", test_auth_set_get_redact_remove, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite auth_lua_suite = {"/auth_lua", tests, NULL, 1,
                             MUNIT_SUITE_OPTION_NONE};
