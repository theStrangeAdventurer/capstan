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

static int write_file(const char *path, const char *content) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return 0;
  size_t length = strlen(content);
  int ok = fwrite(content, 1, length, file) == length;
  if (fclose(file) != 0)
    ok = 0;
  return ok;
}

static int test_rotated_path(const char *path, int index, char *buffer,
                             size_t buffer_size) {
  const char *suffix = ".jsonl";
  size_t length = strlen(path);
  size_t suffix_length = strlen(suffix);
  if (length < suffix_length)
    return 0;
  int count = snprintf(buffer, buffer_size, "%.*s.%d.jsonl",
                       (int)(length - suffix_length), path, index);
  return count >= 0 && (size_t)count < buffer_size;
}

static int write_test_rotation_manifest(const char *path,
                                        const char *transaction) {
  char ready[700];
  snprintf(ready, sizeof(ready), "%s/ready", transaction);
  FILE *file = fopen(ready, "wb");
  if (!file)
    return 0;

  int ok = 1;
  for (int index = 1; index <= 5 && ok; index++) {
    char staged[700];
    char archive[512];
    struct stat st;
    snprintf(staged, sizeof(staged), "%s/%d.jsonl", transaction, index);
    const char *identity_path = staged;
    if (lstat(staged, &st) != 0) {
      if (!test_rotated_path(path, index, archive, sizeof(archive)) ||
          lstat(archive, &st) != 0) {
        ok = 0;
        break;
      }
      identity_path = archive;
    }
    if (lstat(identity_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        fprintf(file, "%d %llu %llu\n", index,
                (unsigned long long)st.st_dev,
                (unsigned long long)st.st_ino) < 0)
      ok = 0;
  }

  char empty[700];
  struct stat empty_st;
  snprintf(empty, sizeof(empty), "%s/empty", transaction);
  if (ok && (lstat(empty, &empty_st) != 0 ||
             !S_ISREG(empty_st.st_mode) ||
             fprintf(file, "0 %llu %llu\n",
                     (unsigned long long)empty_st.st_dev,
                     (unsigned long long)empty_st.st_ino) < 0))
    ok = 0;
  if (fclose(file) != 0)
    ok = 0;
  return ok;
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
  munit_assert_true(log_event("test",
            "Tenant-Id: tenant-secret\n"
            "X-Subscription-Key: subscription-secret\n"
            "Accept: */*"));

  char path[512];
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  char *content = read_file_alloc(path);
  munit_assert_not_null(content);
  munit_assert_null(strstr(content, "tenant-secret"));
  munit_assert_null(strstr(content, "subscription-secret"));
  munit_assert_not_null(strstr(content, "\"schema\":\"capstan.log.v1\""));
  munit_assert_not_null(strstr(content, "Tenant-Id: [REDACTED]\\n"));
  munit_assert_not_null(strstr(content, "X-Subscription-Key: [REDACTED]"));
  munit_assert_not_null(strstr(content, "Accept: */*"));

  struct stat st;
  char log_dir[512];
  snprintf(log_dir, sizeof(log_dir), "%s", path);
  char *slash = strrchr(log_dir, '/');
  munit_assert_not_null(slash);
  *slash = '\0';
  munit_assert_int(stat(log_dir, &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0700);

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
  munit_assert_true(log_event("test", message));

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

static MunitResult test_session_scoped_log_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char state_dir[256];
  snprintf(state_dir, sizeof(state_dir), "/tmp/capstan-log-scoped-test-%ld",
           (long)getpid());
  mkdir(state_dir, 0700);
  munit_assert_int(setenv("XDG_STATE_HOME", state_dir, 1), ==, 0);

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  munit_assert_int(luaL_dostring(L, "capstan = { config = {} }"), ==, LUA_OK);
  munit_assert_true(log_set_session_id("my fucking bench"));
  log_init(L);
  munit_assert_true(log_event("test", "scoped marker"));

  char path[512];
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  munit_assert_not_null(
      strstr(path, "/logs/sessions/my fucking bench/"));
  char *content = read_file_alloc(path);
  munit_assert_not_null(content);
  munit_assert_not_null(strstr(content, "\"session_id\":\"my fucking bench\""));
  munit_assert_not_null(strstr(content, "scoped marker"));

  struct stat st;
  munit_assert_int(stat(path, &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0600);
  char *slash = strrchr(path, '/');
  munit_assert_not_null(slash);
  *slash = '\0';
  munit_assert_int(stat(path, &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0700);

  free(content);
  log_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_log_recovers_committed_rotation(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char state_dir[256];
  snprintf(state_dir, sizeof(state_dir), "/tmp/capstan-log-rotate-test-%ld",
           (long)getpid());
  mkdir(state_dir, 0700);
  munit_assert_int(setenv("XDG_STATE_HOME", state_dir, 1), ==, 0);

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  munit_assert_int(luaL_dostring(L, "capstan = { config = {} }"), ==, LUA_OK);
  log_init(L);
  munit_assert_true(log_event("test", "seed"));

  char path[512];
  char transaction[640];
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  snprintf(transaction, sizeof(transaction), "%s.rotate", path);
  munit_assert_int(mkdir(transaction, 0700), ==, 0);
  munit_assert_true(write_file(path, "current-old\n"));

  const char *snapshots[] = {"current-old\n", "old-1\n", "old-2\n",
                             "old-3\n"};
  for (int index = 1; index <= 4; index++) {
    char staged[700];
    snprintf(staged, sizeof(staged), "%s/%d.jsonl", transaction, index);
    munit_assert_true(write_file(staged, snapshots[index - 1]));
  }
  char archive[512];
  munit_assert_true(test_rotated_path(path, 5, archive, sizeof(archive)));
  munit_assert_true(write_file(archive, "old-4\n"));
  char empty[700];
  snprintf(empty, sizeof(empty), "%s/empty", transaction);
  munit_assert_true(write_file(empty, ""));
  munit_assert_true(write_test_rotation_manifest(path, transaction));

  munit_assert_true(log_event("test", "after recovery"));
  for (int index = 1; index <= 5; index++) {
    const char *expected[] = {"current-old\n", "old-1\n", "old-2\n",
                              "old-3\n", "old-4\n"};
    munit_assert_true(test_rotated_path(path, index, archive,
                                        sizeof(archive)));
    char *content = read_file_alloc(archive);
    munit_assert_not_null(content);
    munit_assert_string_equal(content, expected[index - 1]);
    free(content);
  }
  char *current = read_file_alloc(path);
  munit_assert_not_null(current);
  munit_assert_not_null(strstr(current, "after recovery"));
  munit_assert_null(strstr(current, "current-old"));
  free(current);
  munit_assert_int(access(transaction, F_OK), ==, -1);

  log_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_log_rotates_at_threshold_end_to_end(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char state_dir[256];
  snprintf(state_dir, sizeof(state_dir), "/tmp/capstan-log-threshold-test-%ld",
           (long)getpid());
  mkdir(state_dir, 0700);
  munit_assert_int(setenv("XDG_STATE_HOME", state_dir, 1), ==, 0);

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  munit_assert_int(luaL_dostring(L, "capstan = { config = {} }"), ==, LUA_OK);
  log_init(L);
  munit_assert_true(log_event("test", "before rotation"));

  char path[512];
  char archive[512];
  char transaction[640];
  struct stat st;
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  munit_assert_int(truncate(path, 10L * 1024L * 1024L), ==, 0);
  munit_assert_true(log_event("test", "after threshold rotation"));
  munit_assert_true(test_rotated_path(path, 1, archive, sizeof(archive)));
  munit_assert_int(stat(archive, &st), ==, 0);
  munit_assert_int(st.st_size, ==, 10L * 1024L * 1024L);
  char *current = read_file_alloc(path);
  munit_assert_not_null(current);
  munit_assert_not_null(strstr(current, "after threshold rotation"));
  free(current);
  snprintf(transaction, sizeof(transaction), "%s.rotate", path);
  munit_assert_int(access(transaction, F_OK), ==, -1);

  log_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_log_rejects_symlink_target(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char state_dir[256];
  snprintf(state_dir, sizeof(state_dir), "/tmp/capstan-log-link-test-%ld",
           (long)getpid());
  mkdir(state_dir, 0700);
  munit_assert_int(setenv("XDG_STATE_HOME", state_dir, 1), ==, 0);

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  munit_assert_int(luaL_dostring(L, "capstan = { config = {} }"), ==, LUA_OK);
  log_init(L);
  munit_assert_true(log_event("test", "initial"));
  char path[512];
  munit_assert_int(log_path(path, sizeof(path)), ==, 0);
  munit_assert_int(unlink(path), ==, 0);

  char victim[512];
  snprintf(victim, sizeof(victim), "%s/victim", state_dir);
  FILE *file = fopen(victim, "w");
  munit_assert_not_null(file);
  fputs("preserve-me", file);
  fclose(file);
  munit_assert_int(symlink(victim, path), ==, 0);
  munit_assert_false(log_event("test", "must not follow"));

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "log_read_lock");
  munit_assert_int(lua_pcall(L, 0, 1, 0), ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "log_read_tail");
  lua_pushstring(L, path);
  lua_pushinteger(L, 1024);
  munit_assert_int(lua_pcall(L, 2, 2, 0), !=, LUA_OK);
  lua_pop(L, 1);
  lua_getfield(L, -1, "log_read_unlock");
  munit_assert_int(lua_pcall(L, 0, 0, 0), ==, LUA_OK);
  lua_pop(L, 1);

  char *content = read_file_alloc(victim);
  munit_assert_not_null(content);
  munit_assert_string_equal(content, "preserve-me");
  free(content);
  unlink(path);
  unlink(victim);
  log_cleanup();
  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/event_uses_lua_redaction_config", test_log_event_uses_lua_redaction_config,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/event_preserves_long_messages", test_log_event_preserves_long_messages,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/session_scoped_log_path", test_session_scoped_log_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/recovers_committed_rotation", test_log_recovers_committed_rotation,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rotates_at_threshold_end_to_end",
     test_log_rotates_at_threshold_end_to_end, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_symlink_target", test_log_rejects_symlink_target, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite log_suite = {"/log", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
