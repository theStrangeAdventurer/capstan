#include "dispatch.h"
#include "munit.h"
#include <string.h>

static MunitResult test_no_command_for_plain_text(const MunitParameter params[],
                                                  void *data) {
  (void)params;
  (void)data;
  char command[MAX_COMMAND_LEN];
  size_t end = 99;
  munit_assert_int(has_command("hello /file", command, &end), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_parses_leading_command(const MunitParameter params[],
                                               void *data) {
  (void)params;
  (void)data;
  char command[MAX_COMMAND_LEN];
  size_t end = 0;
  munit_assert_int(has_command("  /new now", command, &end), ==, 1);
  munit_assert_string_equal(command, "/new");
  munit_assert_size(end, ==, 6);
  return MUNIT_OK;
}

static MunitResult test_parses_single_slash_for_command_menu(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char command[MAX_COMMAND_LEN];
  size_t end = 0;
  munit_assert_int(has_command("/", command, &end), ==, 1);
  munit_assert_string_equal(command, "/");
  munit_assert_size(end, ==, 1);
  return MUNIT_OK;
}

static MunitResult test_truncates_long_command(const MunitParameter params[],
                                               void *data) {
  (void)params;
  (void)data;
  char input[128];
  memset(input, 'a', sizeof(input));
  input[0] = '/';
  input[sizeof(input) - 1] = '\0';

  char command[MAX_COMMAND_LEN];
  size_t end = 0;
  munit_assert_int(has_command(input, command, &end), ==, 1);
  munit_assert_size(strlen(command), ==, MAX_COMMAND_LEN - 1);
  munit_assert_size(end, ==, strlen(input));
  return MUNIT_OK;
}

static MunitResult test_blocking_enter_requires_top_level_run(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_false(dispatch_blocking_enter_allowed(0));
  munit_assert_true(dispatch_blocking_enter_allowed(1));
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/no_command_for_plain_text", test_no_command_for_plain_text, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/parses_leading_command", test_parses_leading_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/parses_single_slash_for_command_menu",
     test_parses_single_slash_for_command_menu, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/truncates_long_command", test_truncates_long_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/blocking_enter_requires_top_level_run",
     test_blocking_enter_requires_top_level_run, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite dispatch_suite = {"/dispatch", tests, NULL, 1,
                             MUNIT_SUITE_OPTION_NONE};
