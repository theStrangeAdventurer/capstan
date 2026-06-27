#include "munit.h"
#include "permit_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MunitResult test_pattern_exact_match(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_pattern_match("shell ls", "shell ls"), ==, 1);
  munit_assert_int(permit_pattern_match("shell ls", "shell pwd"), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_pattern_space_star_prefix(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_pattern_match("file_read *", "file_read README.md"),
                   ==, 1);
  munit_assert_int(permit_pattern_match("file_read *", "file_write README.md"),
                   ==, 0);
  return MUNIT_OK;
}

static MunitResult test_pattern_slash_star_prefix(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_pattern_match("/repo/*", "/repo/README.md"), ==, 1);
  munit_assert_int(permit_pattern_match("/repo/*", "/repo/src/main.c"), ==, 1);
  munit_assert_int(permit_pattern_match("/repo/*", "/repo-other/main.c"), ==,
                   0);
  munit_assert_int(permit_pattern_match("/repo/*", "/repo"), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_pattern_single_star_matches_any_target(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_pattern_match("*", "https://example.com"), ==, 1);
  munit_assert_int(permit_pattern_match("*", "/repo"), ==, 1);
  munit_assert_int(permit_pattern_match("*", "shell target"), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_pattern_glob_matches_fetch_domains(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(
      permit_pattern_match("https://api.openai.com/*",
                           "https://api.openai.com/v1/models"),
      ==, 1);
  munit_assert_int(permit_pattern_match("https://api.openai.com/*",
                                        "https://api.openai.com"),
                   ==, 1);
  munit_assert_int(
      permit_pattern_match("https://api.openai.com/*",
                           "https://api.openai.com.evil.test/v1/models"),
      ==, 0);
  munit_assert_int(
      permit_pattern_match("https://*.example.com/*",
                           "https://api.example.com/v1"),
      ==, 1);
  munit_assert_int(
      permit_pattern_match("https://*.example.com/*",
                           "https://example.com/v1"),
      ==, 0);
  munit_assert_int(
      permit_pattern_match("*://api.example.com/*",
                           "http://api.example.com/v1"),
      ==, 1);
  return MUNIT_OK;
}

static MunitResult test_pattern_question_matches_one_character(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_pattern_match("file?.txt", "file1.txt"), ==, 1);
  munit_assert_int(permit_pattern_match("file?.txt", "file12.txt"), ==, 0);
  munit_assert_int(permit_pattern_match("file?.txt", "file.txt"), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_pattern_tilde_expands_home(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char old_home[4096];
  const char *prev_home = getenv("HOME");
  if (prev_home)
    snprintf(old_home, sizeof(old_home), "%s", prev_home);

  munit_assert_int(setenv("HOME", "/Users/tester", 1), ==, 0);
  munit_assert_int(
      permit_pattern_match("~/narnia/tui-agent/*",
                           "/Users/tester/narnia/tui-agent/src/main.c"),
      ==, 1);
  munit_assert_int(
      permit_pattern_match("~/narnia/tui-agent/*",
                           "/Users/tester/narnia/tui-agent"),
      ==, 1);
  munit_assert_int(
      permit_pattern_match("~/narnia/tui-agent/*",
                           "/Users/other/narnia/tui-agent/src/main.c"),
      ==, 0);

  if (prev_home)
    munit_assert_int(setenv("HOME", old_home, 1), ==, 0);
  else
    munit_assert_int(unsetenv("HOME"), ==, 0);

  return MUNIT_OK;
}

static MunitResult test_file_read_allows_workspace_relative_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_file_read_check("/repo", "src/main.c"), ==,
                   PERM_ALLOW);
  return MUNIT_OK;
}

static MunitResult test_file_read_allows_workspace_absolute_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_file_read_check("/repo", "/repo/src/main.c"), ==,
                   PERM_ALLOW);
  return MUNIT_OK;
}

static MunitResult test_file_read_asks_for_parent_escape(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_file_read_check("/repo", "../secret.txt"), ==,
                   PERM_ASK);
  return MUNIT_OK;
}

static MunitResult test_file_read_asks_for_sibling_prefix(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(permit_file_read_check("/repo", "/repo-other/file.txt"), ==,
                   PERM_ASK);
  return MUNIT_OK;
}

static MunitResult test_lua_escape_string_plain(const MunitParameter params[],
                                                void *data) {
  (void)params;
  (void)data;
  char out[64];
  munit_assert_int(permit_lua_escape_string("file_read", out, sizeof(out)), ==,
                   1);
  munit_assert_string_equal(out, "file_read");
  return MUNIT_OK;
}

static MunitResult test_lua_escape_string_special_chars(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char out[128];
  munit_assert_int(
      permit_lua_escape_string("shell \"x\"\\y\nnext\tcol", out, sizeof(out)),
      ==, 1);
  munit_assert_string_equal(out, "shell \\\"x\\\"\\\\y\\nnext\\tcol");
  return MUNIT_OK;
}

static MunitResult test_lua_escape_string_reports_truncation(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char out[4];
  munit_assert_int(permit_lua_escape_string("abcd", out, sizeof(out)), ==, 0);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/pattern_exact_match", test_pattern_exact_match, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/pattern_space_star_prefix", test_pattern_space_star_prefix, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/pattern_slash_star_prefix", test_pattern_slash_star_prefix, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/pattern_single_star_matches_any_target",
     test_pattern_single_star_matches_any_target, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/pattern_glob_matches_fetch_domains",
     test_pattern_glob_matches_fetch_domains, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/pattern_question_matches_one_character",
     test_pattern_question_matches_one_character, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/pattern_tilde_expands_home", test_pattern_tilde_expands_home, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_allows_workspace_relative_path",
     test_file_read_allows_workspace_relative_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_allows_workspace_absolute_path",
     test_file_read_allows_workspace_absolute_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_asks_for_parent_escape",
     test_file_read_asks_for_parent_escape, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_asks_for_sibling_prefix",
     test_file_read_asks_for_sibling_prefix, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/lua_escape_string_plain", test_lua_escape_string_plain, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/lua_escape_string_special_chars", test_lua_escape_string_special_chars,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/lua_escape_string_reports_truncation",
     test_lua_escape_string_reports_truncation, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite permit_logic_suite = {"/permit_logic", tests, NULL, 1,
                                 MUNIT_SUITE_OPTION_NONE};
