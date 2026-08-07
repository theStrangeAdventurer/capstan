#include "diff_highlight.h"
#include "munit.h"

static MunitResult test_long_wrapped_diff_lines_keep_kind(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *added = "+a very long added line that spans screen rows";
  const char *deleted = "-a very long deleted line that spans screen rows";
  const char *wrapped_add_segment = added + 12;
  const char *wrapped_delete_segment = deleted + 12;
  munit_assert_char(wrapped_add_segment[0], !=, '+');
  munit_assert_char(wrapped_delete_segment[0], !=, '-');
  munit_assert_int(diff_highlight_kind(wrapped_add_segment), ==,
                   DIFF_HIGHLIGHT_NONE);
  munit_assert_int(diff_highlight_kind(wrapped_delete_segment), ==,
                   DIFF_HIGHLIGHT_NONE);
  munit_assert_int(diff_highlight_kind(added), ==, DIFF_HIGHLIGHT_ADD);
  munit_assert_int(diff_highlight_kind(deleted), ==, DIFF_HIGHLIGHT_DELETE);
  return MUNIT_OK;
}

static MunitResult test_diff_headers_are_not_highlighted(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  munit_assert_int(diff_highlight_kind("+++ b/file"), ==,
                   DIFF_HIGHLIGHT_NONE);
  munit_assert_int(diff_highlight_kind("--- a/file"), ==,
                   DIFF_HIGHLIGHT_NONE);
  munit_assert_int(diff_highlight_kind(" context"), ==,
                   DIFF_HIGHLIGHT_NONE);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/long_wrapped_lines_keep_kind", test_long_wrapped_diff_lines_keep_kind,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/headers_are_not_highlighted", test_diff_headers_are_not_highlighted,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite diff_highlight_suite = {
    "/diff_highlight", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
