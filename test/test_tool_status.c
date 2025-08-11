#include "munit.h"
#include "tool_status.h"
#include <string.h>

static MunitResult test_tool_status_boundaries(const MunitParameter params[],
                                                void *data) {
  (void)params;
  (void)data;

  munit_assert_true(tool_status_starts_line("⚙ shell", (int)strlen("⚙ shell")));
  munit_assert_true(tool_status_starts_line("  $ make test", 13));
  munit_assert_false(tool_status_starts_line("if pid == 0:", 12));

  munit_assert_false(tool_status_ends_line("", 0));
  munit_assert_false(tool_status_ends_line("buf = bytearray()", 17));
  munit_assert_true(tool_status_ends_line("— done", (int)strlen("— done")));
  munit_assert_true(tool_status_ends_line(
      "  $ make && git diff --check — done",
      (int)strlen("  $ make && git diff --check — done")));
  munit_assert_true(tool_status_ends_line("— error: failed",
                                          (int)strlen("— error: failed")));
  munit_assert_true(tool_status_ends_line(
      "  $ false — error: [exit 1]",
      (int)strlen("  $ false — error: [exit 1]")));
  munit_assert_true(tool_status_ends_line(
      "⚙ file_read: invalid arguments — error",
      (int)strlen("⚙ file_read: invalid arguments — error")));
  munit_assert_true(tool_status_ends_line(
      "— denied by user", (int)strlen("— denied by user")));
  munit_assert_true(tool_status_ends_line(
      "— skipped redundant check",
      (int)strlen("— skipped redundant check")));
  munit_assert_false(tool_status_ends_line(
      "— errorish", (int)strlen("— errorish")));
  munit_assert_false(tool_status_ends_line(
      "text with em dash — but no status",
      (int)strlen("text with em dash — but no status")));
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/boundaries", test_tool_status_boundaries, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite tool_status_suite = {
    "/tool_status", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
