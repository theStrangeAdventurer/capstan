#include "munit.h"
#include "session.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_temp[PATH_MAX];
static char *g_old_xdg;

static void *setup(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *old = getenv("XDG_STATE_HOME");
  g_old_xdg = old ? strdup(old) : NULL;
  snprintf(g_temp, sizeof(g_temp), "/tmp/capstan-session-test-XXXXXX");
  int fd = mkstemp(g_temp);
  munit_assert_int(fd, >=, 0);
  close(fd);
  unlink(g_temp);
  munit_assert_int(mkdir(g_temp, 0700), ==, 0);
  setenv("XDG_STATE_HOME", g_temp, 1);
  return NULL;
}

static void teardown(void *fixture) {
  (void)fixture;
  char command[PATH_MAX + 16];
  snprintf(command, sizeof(command), "rm -rf '%s'", g_temp);
  system(command);
  if (g_old_xdg) {
    setenv("XDG_STATE_HOME", g_old_xdg, 1);
    free(g_old_xdg);
  } else {
    unsetenv("XDG_STATE_HOME");
  }
  g_old_xdg = NULL;
}

static MunitResult test_round_trip(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/один"));
  Session session;
  munit_assert_true(session_create(&session));
  snprintf(session.title, sizeof(session.title), "Тестовая сессия");
  session.title_generated = 1;
  SessionMessage messages[] = {
      {SESSION_ROLE_USER, "привет\n\"мир\"", "raw\\user\ncontext"},
      {SESSION_ROLE_ASSISTANT, "ответ", "ответ"},
      {SESSION_ROLE_ASSISTANT, "", ""},
  };
  session.messages = messages;
  session.message_count = 3;
  session.updated_at += 5;
  munit_assert_true(session_save(&session));

  Session loaded;
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_string_equal(loaded.title, "Тестовая сессия");
  munit_assert_true(loaded.title_generated);
  munit_assert_size(loaded.message_count, ==, 2);
  munit_assert_int(loaded.messages[0].role, ==, SESSION_ROLE_USER);
  munit_assert_string_equal(loaded.messages[0].text, "привет\n\"мир\"");
  munit_assert_string_equal(loaded.messages[0].raw_text,
                            "raw\\user\ncontext");
  munit_assert_string_equal(loaded.messages[1].text, "ответ");
  session_free(&loaded);
  session.messages = NULL;
  session.message_count = 0;
  return MUNIT_OK;
}

static MunitResult test_workspace_and_active(const MunitParameter params[],
                                             void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/one"));
  char first_dir[PATH_MAX];
  snprintf(first_dir, sizeof(first_dir), "%s", session_store_dir());
  Session first;
  munit_assert_true(session_create(&first));
  char active[SESSION_ID_SIZE];
  munit_assert_true(session_get_active(active, sizeof(active)));
  munit_assert_string_equal(active, first.id);

  munit_assert_true(session_store_init("/repo/two"));
  munit_assert_string_not_equal(first_dir, session_store_dir());
  munit_assert_false(session_get_active(active, sizeof(active)));
  session_free(&first);
  return MUNIT_OK;
}

static MunitResult test_list_sorted(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/list"));
  Session older, newer;
  munit_assert_true(session_create(&older));
  snprintf(older.title, sizeof(older.title), "Older");
  older.updated_at = 10;
  munit_assert_true(session_save(&older));
  munit_assert_true(session_create(&newer));
  snprintf(newer.title, sizeof(newer.title), "Newer");
  newer.updated_at = 20;
  munit_assert_true(session_save(&newer));
  SessionInfo *items;
  size_t count;
  munit_assert_true(session_list(&items, &count));
  munit_assert_size(count, ==, 2);
  munit_assert_string_equal(items[0].title, "Newer");
  munit_assert_string_equal(items[1].title, "Older");
  session_list_free(items);
  session_free(&older);
  session_free(&newer);
  return MUNIT_OK;
}

static MunitResult test_permissions_and_corruption(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/perms"));
  struct stat st;
  munit_assert_int(stat(session_store_dir(), &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0700);
  Session session;
  munit_assert_true(session_create(&session));
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s.jsonl", session_store_dir(), session.id);
  munit_assert_int(stat(path, &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0600);
  FILE *f = fopen(path, "wb");
  munit_assert_not_null(f);
  fputs("{\"version\":999}\n", f);
  fclose(f);
  Session loaded;
  munit_assert_false(session_load(session.id, &loaded));
  session_free(&session);
  return MUNIT_OK;
}

static MunitResult test_oversized_line_fails_closed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/oversized"));
  Session session;
  munit_assert_true(session_create(&session));
  SessionMessage messages[] = {
      {SESSION_ROLE_USER, "valid prefix", "valid prefix"},
  };
  session.messages = messages;
  session.message_count = 1;
  munit_assert_true(session_save(&session));

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s.jsonl", session_store_dir(), session.id);
  FILE *f = fopen(path, "ab");
  munit_assert_not_null(f);
  size_t oversized_len = 4 * 1024 * 1024 + 1;
  char *oversized = malloc(oversized_len);
  munit_assert_not_null(oversized);
  memset(oversized, 'x', oversized_len);
  munit_assert_size(fwrite(oversized, 1, oversized_len, f), ==,
                    oversized_len);
  munit_assert_int(fputc('\n', f), !=, EOF);
  free(oversized);
  munit_assert_int(fclose(f), ==, 0);

  Session loaded;
  munit_assert_false(session_load(session.id, &loaded));
  munit_assert_null(loaded.messages);
  munit_assert_size(loaded.message_count, ==, 0);

  session.messages = NULL;
  session.message_count = 0;
  session_free(&session);
  return MUNIT_OK;
}

static MunitResult test_title(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char title[24];
  session_title_from_text("  hello\n  session title that is long", title,
                          sizeof(title));
  munit_assert_string_equal(title, "hello session title …");
  session_title_from_text("длинное название сессии", title, sizeof(title));
  munit_assert_string_equal(title, "длинное на…");
  session_title_from_text("\n\t", title, sizeof(title));
  munit_assert_string_equal(title, "New session");
  return MUNIT_OK;
}

static MunitResult test_named_session_is_exact_and_not_active(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/named"));
  munit_assert_true(session_id_valid("my fucking bench"));
  munit_assert_true(session_id_valid("мой бенч"));
  munit_assert_false(session_id_valid(" leading"));
  munit_assert_false(session_id_valid("trailing "));
  munit_assert_false(session_id_valid("../escape"));
  munit_assert_false(session_id_valid("\xc3\x28"));

  Session session;
  munit_assert_true(session_create_named(&session, "my fucking bench"));
  munit_assert_string_equal(session.id, "my fucking bench");
  munit_assert_string_equal(session.title, "my fucking bench");
  munit_assert_true(session.title_generated);

  char active[SESSION_ID_SIZE];
  munit_assert_false(session_get_active(active, sizeof(active)));

  Session duplicate;
  munit_assert_false(session_create_named(&duplicate, "my fucking bench"));

  Session loaded;
  munit_assert_true(session_load("my fucking bench", &loaded));
  munit_assert_string_equal(loaded.title, "my fucking bench");
  munit_assert_true(loaded.title_generated);
  session_free(&loaded);
  session_free(&session);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/round_trip", test_round_trip, setup, teardown, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/workspace_and_active", test_workspace_and_active, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/list_sorted", test_list_sorted, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/permissions_and_corruption", test_permissions_and_corruption, setup,
     teardown, MUNIT_TEST_OPTION_NONE, NULL},
    {"/oversized_line_fails_closed", test_oversized_line_fails_closed, setup,
     teardown, MUNIT_TEST_OPTION_NONE, NULL},
    {"/title", test_title, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL},
    {"/named_session_is_exact_and_not_active",
     test_named_session_is_exact_and_not_active, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite session_suite = {"/session", tests, NULL, 1,
                            MUNIT_SUITE_OPTION_NONE};
