#include "log.h"
#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_file_alloc(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)size, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

static MunitResult test_log_event_uses_lua_redaction_config(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char state_dir[256];
  snprintf(state_dir, sizeof(state_dir), "/tmp/capstan-log-test-%ld",
           (long)getpid());
  mkdir(state_dir, 0700);
  munit_assert_int(setenv("XDG_STATE_HOME", state_dir, 1), ==, 0);

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  int rc = luaL_dostring(L,
                         "capstan = { config = { redaction = {"
                         "names = {'tenant-id'},"
                         "} } }");
  munit_assert_int(rc, ==, LUA_OK);

  log_init(L);
  log_event("test",
            "Tenant-Id: tenant-secret\n"
            "X-Subscription-Key: subscription-secret\n"
            "Accept: */*");

  char path[512];
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  char *content = read_file_alloc(path);
  munit_assert_not_null(content);
  munit_assert_null(strstr(content, "tenant-secret"));
  munit_assert_null(strstr(content, "subscription-secret"));
  munit_assert_not_null(strstr(content, "Tenant-Id: [REDACTED]"));
  munit_assert_not_null(strstr(content, "X-Subscription-Key: [REDACTED]"));
  munit_assert_not_null(strstr(content, "Accept: */*"));

  free(content);
  log_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_log_event_preserves_long_messages(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char state_dir[256];
  snprintf(state_dir, sizeof(state_dir), "/tmp/capstan-log-long-test-%ld",
           (long)getpid());
  mkdir(state_dir, 0700);
  munit_assert_int(setenv("XDG_STATE_HOME", state_dir, 1), ==, 0);

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  int rc = luaL_dostring(L, "capstan = { config = {} }");
  munit_assert_int(rc, ==, LUA_OK);
  log_init(L);

  size_t len = 3500;
  char *message = malloc(len + 64);
  munit_assert_not_null(message);
  memset(message, 'a', len);
  strcpy(message + len, " tail-marker");
  log_event("test", message);

  char path[512];
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  char *content = read_file_alloc(path);
  munit_assert_not_null(content);
  munit_assert_not_null(strstr(content, "tail-marker"));

  free(content);
  free(message);
  log_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/event_uses_lua_redaction_config", test_log_event_uses_lua_redaction_config,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/event_preserves_long_messages", test_log_event_preserves_long_messages,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite log_suite = {"/log", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
