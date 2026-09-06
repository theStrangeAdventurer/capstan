#include "munit.h"
#include "tui_layout.h"
#include "text_layout.h"
#include <locale.h>
#include <string.h>

static MunitResult test_input_hit_area(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;

  munit_assert_true(tui_layout_point_in_input(24, 80, 19, 1));
  munit_assert_true(tui_layout_point_in_input(24, 80, 22, 78));
  munit_assert_false(tui_layout_point_in_input(24, 80, 18, 1));
  munit_assert_false(tui_layout_point_in_input(24, 80, 19, 0));
  munit_assert_false(tui_layout_point_in_input(24, 80, 23, 1));
  munit_assert_false(tui_layout_point_in_input(4, 80, 0, 1));
  return MUNIT_OK;
}

static MunitResult test_workspace_footer(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  setlocale(LC_CTYPE, "");
  TuiWorkspaceFooter footer;
  WorkspaceStatus status = {.state = WORKSPACE_STATUS_READY, .files = 3,
                            .added = 128, .deleted = 34};
  tui_layout_workspace_footer(80, "/home/me/project/src", "/home/me", &status, &footer);
  munit_assert_string_equal(footer.path, "~/project/src");
  munit_assert_string_equal(footer.files, "3 files · ");
  munit_assert_string_equal(footer.added, "+128");
  munit_assert_string_equal(footer.deleted, " −34");
  tui_layout_workspace_footer(30, "/home/me/long/project/src", "/home/me", &status, &footer);
  munit_assert_string_equal(footer.files, "");
  munit_assert_string_equal(footer.added, "+128");
  munit_assert_not_null(strstr(footer.path, "…"));
  munit_assert_not_null(strstr(footer.path, "/src"));
  for (int width = 7; width < 100; width++) {
    tui_layout_workspace_footer(width, "/home/me/目录/проект/src", "/home/me", &status, &footer);
    int used = text_columns(footer.path, strlen(footer.path)) + 6;
    if (footer.summary_width) used += footer.summary_width + 3;
    munit_assert_int(used, <=, width);
  }
  status = (WorkspaceStatus){.state = WORKSPACE_STATUS_READY};
  tui_layout_workspace_footer(80, "/home/meeting/project", "/home/me", &status, &footer);
  munit_assert_string_equal(footer.path, "/home/meeting/project");
  munit_assert_string_equal(footer.files, "clean");
  status.state = WORKSPACE_STATUS_NONE;
  tui_layout_workspace_footer(80, "/tmp/a\nb\033c", NULL, &status, &footer);
  munit_assert_string_equal(footer.path, "/tmp/a?b?c");
  munit_assert_int(footer.summary_width, ==, 0);
  status.state = WORKSPACE_STATUS_ERROR;
  tui_layout_workspace_footer(80, "/project", NULL, &status, &footer);
  munit_assert_string_equal(footer.files, "diff unavailable");
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/workspace_footer", test_workspace_footer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/input_hit_area", test_input_hit_area, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite tui_layout_suite = {
    "/tui_layout", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
