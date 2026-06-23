#include "munit.h"
#include "permit_logic.h"

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
