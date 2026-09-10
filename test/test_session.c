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
  SessionImage images[] = {{"image/png", "YWJj"}};
  SessionMessage messages[] = {
      {SESSION_ROLE_USER, "привет\n\"мир\"", "raw\\user\ncontext", images,
       1, {0}},
      {SESSION_ROLE_ASSISTANT, "ответ", "ответ", NULL, 0, {0}},
      {SESSION_ROLE_ASSISTANT, "", "", NULL, 0, {0}},
  };
  session.messages = messages;
  session.message_count = 3;
  session.usage = (UsageStats){30000, 1234, 31234, 32768};
  session.updated_at += 5;
  munit_assert_true(session_save(&session));

  Session loaded;
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_string_equal(loaded.title, "Тестовая сессия");
  munit_assert_int(loaded.usage.prompt_tokens, ==, 30000);
  munit_assert_int(loaded.usage.completion_tokens, ==, 1234);
  munit_assert_int(loaded.usage.total_tokens, ==, 31234);
  munit_assert_int(loaded.usage.context_limit, ==, 32768);
  munit_assert_true(loaded.title_generated);
  munit_assert_size(loaded.message_count, ==, 2);
  munit_assert_int(loaded.messages[0].role, ==, SESSION_ROLE_USER);
  munit_assert_string_equal(loaded.messages[0].text, "привет\n\"мир\"");
  munit_assert_string_equal(loaded.messages[0].raw_text,
                            "raw\\user\ncontext");
  munit_assert_string_equal(loaded.messages[1].text, "ответ");
  munit_assert_size(loaded.messages[0].image_count, ==, 1);
  munit_assert_string_equal(loaded.messages[0].images[0].mime_type,
                            "image/png");
  munit_assert_string_equal(loaded.messages[0].images[0].data, "YWJj");
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
  int created = 1;
  munit_assert_false(
      session_load_or_create_named(&loaded, session.id, &created));
  munit_assert_false(created);
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
      {SESSION_ROLE_USER, "valid prefix", "valid prefix", NULL, 0, {0}},
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

static MunitResult test_incomplete_image_chunk_fails_closed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/incomplete-image"));
  Session session;
  munit_assert_true(session_create(&session));
  SessionMessage messages[] = {
      {SESSION_ROLE_USER, "image", "image", NULL, 0, {0}},
  };
  session.messages = messages;
  session.message_count = 1;
  munit_assert_true(session_save(&session));

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s.jsonl", session_store_dir(), session.id);
  FILE *f = fopen(path, "ab");
  munit_assert_not_null(f);
  fputs("{\"type\":\"image\",\"index\":0,\"mime_type\":\"image/png\","
        "\"data\":\"iVBORw0K\",\"final\":0}\n", f);
  munit_assert_int(fclose(f), ==, 0);

  Session loaded;
  munit_assert_false(session_load(session.id, &loaded));
  munit_assert_null(loaded.messages);
  session.messages = NULL;
  session.message_count = 0;
  session_free(&session);
  return MUNIT_OK;
}

static MunitResult test_json_validation(const MunitParameter params[],
                                         void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/json-validation"));
  const char *header = " { \"version\" : 1, \"id\" : \"strict\", \"title\" : \"Legacy\" }\n";
  const char *bad_rows[] = {
      "{\"version\":1,\"id\":\"strict\",\"title\":\"Legacy\"",
      "{\"version\":1garbage,\"id\":\"strict\",\"title\":\"Legacy\"}",
      "{\"role\":\"user\",\"text\":\"hello\",\"raw_text\":\"hello\"",
      "{\"role\":\"user\",\"text\":\"hello\",\"raw_text\":\"hello\"}garbage",
      "{\"role\":\"user\",\"text\":\"\\q\",\"raw_text\":\"hello\"}",
      "{\"role\":\"user\",\"text\":\"\\u12\",\"raw_text\":\"hello\"}",
      "{\"role\":\"user\",\"text\":\"hello\",\"raw_text\":\"hello\",}",
      "{\"role\":\"user\",\"text\":\"hello\",\"raw_text\":\"hello\",\"extra\":[1,]}",
      "{\"type\":\"image\",\"index\":0,\"mime_type\":\"image/png\",\"data\":\"YWJj\",\"final\":1garbage}",
      "{\"extra\":{\"role\":\"user\"},\"text\":\"hello\",\"raw_text\":\"hello\"}",
      "{\"role\":\"user\",\"text\":\"hello\",\"raw_text\":\"hello\"}",
  };
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/strict.jsonl", session_store_dir());
  for (size_t i = 0; i < sizeof(bad_rows) / sizeof(bad_rows[0]); i++) {
    char original[1024];
    int len = snprintf(original, sizeof(original), "%s%s%s",
                       i < 2 ? "" : header,
                       i == 8 ? "{\"role\":\"user\",\"text\":\"image\","
                                "\"raw_text\":\"image\"}\n" : "",
                       bad_rows[i]);
    /* A NUL must not hide bytes after an otherwise valid row. */
    if (i + 1 == sizeof(bad_rows) / sizeof(bad_rows[0])) {
      original[len++] = '\0';
      original[len++] = 'x';
    }
    FILE *f = fopen(path, "wb");
    munit_assert_not_null(f);
    munit_assert_size(fwrite(original, 1, (size_t)len, f), ==, (size_t)len);
    munit_assert_int(fclose(f), ==, 0);
    Session loaded;
    munit_assert_false(session_load("strict", &loaded));
    munit_assert_null(loaded.messages);
    munit_assert_size(loaded.message_count, ==, 0);
    munit_assert_string_equal(loaded.id, "");
    int created = 1;
    munit_assert_false(session_load_or_create_named(&loaded, "strict", &created));
    munit_assert_false(created);
    munit_assert_false(session_save(&loaded));
    SessionInfo *items = NULL;
    size_t count = 0;
    munit_assert_true(session_list(&items, &count));
    munit_assert_size(count, ==, 0);
    session_list_free(items);
    f = fopen(path, "rb");
    munit_assert_not_null(f);
    char unchanged[1024];
    munit_assert_size(fread(unchanged, 1, sizeof(unchanged), f), ==, (size_t)len);
    munit_assert_memory_equal((size_t)len, original, unchanged);
    munit_assert_int(fclose(f), ==, 0);
  }
  const char *bad_versions[] = {"1.5", "1e2", "999999999999999999999999"};
  for (size_t i = 0; i < sizeof(bad_versions) / sizeof(bad_versions[0]); i++) {
    FILE *f = fopen(path, "wb");
    munit_assert_not_null(f);
    fprintf(f, "{\"version\":%s,\"id\":\"strict\",\"title\":\"Legacy\"}",
            bad_versions[i]);
    munit_assert_int(fclose(f), ==, 0);
    Session loaded;
    munit_assert_false(session_load("strict", &loaded));
  }
  /* Legacy metadata omits optional fields; blank rows and final EOF remain OK. */
  FILE *f = fopen(path, "wb");
  munit_assert_not_null(f);
  fputs(header, f);
  fputs("\n{\"role\":\"user\",\"text\":\"hello\",\"raw_text\":\"hello\","
        "\"extra\": [true, false, null, {\"n\": -1.25e+2}]}\t", f);
  munit_assert_int(fclose(f), ==, 0);
  Session loaded;
  munit_assert_true(session_load("strict", &loaded));
  munit_assert_false(loaded.title_generated);
  munit_assert_size(loaded.message_count, ==, 1);
  munit_assert_true(session_save(&loaded));
  session_free(&loaded);
  munit_assert_true(session_load("strict", &loaded));
  munit_assert_string_equal(loaded.title, "Legacy");
  munit_assert_string_equal(loaded.messages[0].text, "hello");
  session_free(&loaded);
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

static MunitResult test_load_or_create_named(const MunitParameter params[],
                                              void *data) {
  (void)params;
  (void)data;
  munit_assert_true(session_store_init("/repo/load-or-create"));

  Session created_session;
  int created = 0;
  munit_assert_true(session_load_or_create_named(
      &created_session, "stable key", &created));
  munit_assert_true(created);
  munit_assert_string_equal(created_session.id, "stable key");
  snprintf(created_session.title, sizeof(created_session.title), "Kept title");
  munit_assert_true(session_save(&created_session));
  session_free(&created_session);

  Session loaded;
  created = 1;
  munit_assert_true(
      session_load_or_create_named(&loaded, "stable key", &created));
  munit_assert_false(created);
  munit_assert_string_equal(loaded.title, "Kept title");
  session_free(&loaded);
  return MUNIT_OK;
}

static MunitResult test_shell_ranges(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  munit_assert_true(session_store_init("/repo/shell-ranges"));
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/ranges.jsonl", session_store_dir());
  const char *ranges[] = {
      "{\"type\":\"shell_output\",\"start\":0,\"end\":5}",
      "{\"type\":\"shell_output\",\"start\":-1,\"end\":5}",
      "{\"type\":\"shell_output\",\"start\":0,\"end\":6}",
      "{\"type\":\"shell_output\",\"start\":0.5,\"end\":5}",
      "{\"type\":\"shell_output\",\"start\":5,\"end\":5}",
      ("{\"type\":\"shell_output\",\"start\":0,\"end\":5}\n"
       "{\"type\":\"shell_output\",\"start\":1,\"end\":5}"),
  };
  for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
    FILE *f = fopen(path, "wb");
    munit_assert_not_null(f);
    fputs("{\"version\":1,\"id\":\"ranges\",\"title\":\"Ranges\"}\n"
          "{\"role\":\"assistant\",\"text\":\"hello\",\"raw_text\":\"raw\"}\n", f);
    fputs(ranges[i], f);
    munit_assert_int(fclose(f), ==, 0);
    Session loaded;
    munit_assert_int(session_load("ranges", &loaded), ==, i == 0);
    if (i == 0) {
      munit_assert_size(loaded.messages[0].shell_output.count, ==, 1);
      munit_assert_true(session_save(&loaded));
    } else {
      munit_assert_null(loaded.messages);
    }
    session_free(&loaded);
  }
  return MUNIT_OK;
}

static MunitResult test_tasks_persistence(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  munit_assert_true(session_store_init("/repo/tasks"));
  Session session, loaded;
  munit_assert_true(session_create(&session));
  /* An omitted optional header is the legacy empty state. */
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_null(loaded.tasks_json);
  munit_assert_int(loaded.tasks_view, ==, 0);
  session_free(&loaded);
  for (int view = 0; view <= 3; view++) {
    session.tasks_view = view;
    session.tasks_scroll = 17;
    munit_assert_true(session_save(&session));
    munit_assert_true(session_load(session.id, &loaded));
    munit_assert_int(loaded.tasks_view, ==, view == 3 ? 0 : view);
    munit_assert_int(loaded.tasks_scroll, ==, 0);
    session_free(&loaded);
  }
  const char *plan = "[{\"title\":\"готово\\next\",\"done\":false}]\n\t\001";
  session.tasks_json = malloc(strlen(plan) + 1);
  munit_assert_not_null(session.tasks_json);
  strcpy(session.tasks_json, plan);
  munit_assert_true(session_save(&session));
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_string_equal(loaded.tasks_json, plan);
  session_free(&loaded);
  munit_assert_null(loaded.tasks_json);
  free(session.tasks_json);
  session.tasks_json = malloc(SESSION_TASKS_MAX_BYTES + 2);
  munit_assert_not_null(session.tasks_json);
  memset(session.tasks_json, 'x', SESSION_TASKS_MAX_BYTES + 1);
  session.tasks_json[SESSION_TASKS_MAX_BYTES + 1] = '\0';
  munit_assert_false(session_save(&session));
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_string_equal(loaded.tasks_json, plan);
  session_free(&loaded);
  session.tasks_json[SESSION_TASKS_MAX_BYTES] = '\0';
  munit_assert_true(session_save(&session));
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_size(strlen(loaded.tasks_json), ==, SESSION_TASKS_MAX_BYTES);
  session_free(&loaded);
  session.tasks_json[0] = '\0';
  munit_assert_true(session_save(&session));
  munit_assert_true(session_load(session.id, &loaded));
  munit_assert_string_equal(loaded.tasks_json, "");
  session_free(&loaded);
  session_free(&session);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/tasks_persistence", test_tasks_persistence, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_ranges", test_shell_ranges, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL},
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
    {"/incomplete_image_chunk_fails_closed",
     test_incomplete_image_chunk_fails_closed, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/json_validation", test_json_validation, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/title", test_title, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL},
    {"/named_session_is_exact_and_not_active",
     test_named_session_is_exact_and_not_active, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/load_or_create_named", test_load_or_create_named, setup, teardown,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite session_suite = {"/session", tests, NULL, 1,
                            MUNIT_SUITE_OPTION_NONE};
