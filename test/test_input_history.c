#include "munit.h"
#include "input_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_tmp_state(char *buf, size_t size, const char *name) {
  snprintf(buf, size, "/tmp/capstan-%s-%ld", name, (long)getpid());
  rmdir(buf);
  munit_assert_int(mkdir(buf, 0700), ==, 0);
  munit_assert_int(setenv("XDG_STATE_HOME", buf, 1), ==, 0);
  munit_assert_int(setenv("HOME", "/tmp", 1), ==, 0);
}

static void cleanup_path_tree(const char *state_root) {
  char history_dir[4096];
  snprintf(history_dir, sizeof(history_dir), "%s/capstan/history", state_root);
  const char *path = input_history_path();
  if (path && path[0])
    unlink(path);
  rmdir(history_dir);
  char capstan_dir[4096];
  snprintf(capstan_dir, sizeof(capstan_dir), "%s/capstan", state_root);
  rmdir(capstan_dir);
  rmdir(state_root);
}

static MunitResult test_persists_per_workspace(const MunitParameter params[],
                                               void *data) {
  (void)params;
  (void)data;
  char state[4096];
  make_tmp_state(state, sizeof(state), "input-history-persist");

  munit_assert_true(input_history_load("/repo/one"));
  munit_assert_true(input_history_add("first prompt"));
  munit_assert_true(input_history_add("second\nprompt"));
  char path_one[4096];
  snprintf(path_one, sizeof(path_one), "%s", input_history_path());
  munit_assert_true(path_one[0] != '\0');

  munit_assert_true(input_history_load("/repo/two"));
  munit_assert_int(input_history_count(), ==, 0);
  char path_two[4096];
  snprintf(path_two, sizeof(path_two), "%s", input_history_path());
  munit_assert_true(strcmp(path_one, path_two) != 0);
  unlink(path_two);

  munit_assert_true(input_history_load("/repo/one"));
  munit_assert_int(input_history_count(), ==, 2);
  munit_assert_string_equal(input_history_entry(0), "first prompt");
  munit_assert_string_equal(input_history_entry(1), "second\nprompt");

  cleanup_path_tree(state);
  input_history_reset();
  return MUNIT_OK;
}

static MunitResult test_dedup_and_limit(const MunitParameter params[],
                                        void *data) {
  (void)params;
  (void)data;
  char state[4096];
  make_tmp_state(state, sizeof(state), "input-history-limit");

  munit_assert_true(input_history_load("/repo/project"));
  munit_assert_true(input_history_add("same"));
  munit_assert_true(input_history_add("same"));
  munit_assert_int(input_history_count(), ==, 1);

  for (int i = 0; i < INPUT_HISTORY_LIMIT + 5; i++) {
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "prompt-%02d", i);
    munit_assert_true(input_history_add(prompt));
  }
  munit_assert_int(input_history_count(), ==, INPUT_HISTORY_LIMIT);
  munit_assert_string_equal(input_history_entry(0), "prompt-05");
  munit_assert_string_equal(input_history_entry(INPUT_HISTORY_LIMIT - 1),
                            "prompt-24");

  cleanup_path_tree(state);
  input_history_reset();
  return MUNIT_OK;
}

static MunitResult test_navigation_restores_draft(const MunitParameter params[],
                                                  void *data) {
  (void)params;
  (void)data;
  char state[4096];
  make_tmp_state(state, sizeof(state), "input-history-nav");

  munit_assert_true(input_history_load("/repo/project"));
  munit_assert_true(input_history_add("one"));
  munit_assert_true(input_history_add("two"));
  munit_assert_string_equal(input_history_prev("draft"), "two");
  munit_assert_string_equal(input_history_prev("two"), "one");
  munit_assert_string_equal(input_history_prev("one"), "one");
  munit_assert_string_equal(input_history_next("one"), "two");
  munit_assert_string_equal(input_history_next("two"), "draft");
  munit_assert_string_equal(input_history_next("draft"), "draft");

  cleanup_path_tree(state);
  input_history_reset();
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/persists_per_workspace", test_persists_per_workspace, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/dedup_and_limit", test_dedup_and_limit, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/navigation_restores_draft", test_navigation_restores_draft, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite input_history_suite = {"/input_history", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
