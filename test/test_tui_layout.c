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
  munit_assert_string_equal(footer.files, "Changes: 3 files · ");
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

static MunitResult test_tasks_viewport(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  munit_assert_int(tui_layout_tasks_height(24, 10, 0), ==, 0);
  munit_assert_int(tui_layout_tasks_height(24, 0, 1), ==, 0);
  munit_assert_int(tui_layout_tasks_height(2, 10, 1), ==, 0);
  munit_assert_int(tui_layout_tasks_height(3, 10, 1), ==, 2);
  munit_assert_int(tui_layout_tasks_height(24, 1, 1), ==, 2);
  munit_assert_int(tui_layout_tasks_height(24, 100, 1), ==, 9);
  for (int available = 3; available < 100; available++) {
    int height = tui_layout_tasks_height(available, 100, 1);
    munit_assert_int(height, <, available);
    munit_assert_int(height, <=, 9);
  }
  munit_assert_int(tui_layout_tasks_scroll(0, -3, 20, 8), ==, 0);
  munit_assert_int(tui_layout_tasks_scroll(0, 8, 20, 8), ==, 8);
  munit_assert_int(tui_layout_tasks_scroll(8, 8, 20, 8), ==, 12);
  munit_assert_int(tui_layout_tasks_scroll(12, 0, 3, 8), ==, 0);
  munit_assert_int(tui_layout_tasks_scroll(12, -8, 20, 8), ==, 4);
  return MUNIT_OK;
}

static MunitResult test_session_header(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  setlocale(LC_CTYPE, "");
  munit_assert_int(tui_layout_session_height(30, 80), ==, 2);
  munit_assert_int(tui_layout_session_height(8, 80), ==, 0);
  munit_assert_int(tui_layout_session_height(30, 6), ==, 0);
  TuiSessionRow row;
  for (int cols = 7; cols < 120; cols++) {
    tui_layout_session_row(cols, "目录 é Очень длинное имя сессии", "⧉", &row);
    munit_assert_int(row.x, >=, 1);
    munit_assert_int(row.x + row.width, ==, cols - 1);
    munit_assert_int(row.width, <=, 60);
    munit_assert_int(row.width, ==, text_columns(row.text, strlen(row.text)));
  }
  tui_layout_session_row(80, "raw\nname\033", "[]", &row);
  munit_assert_string_equal(row.text, "raw name  []");
  tui_layout_session_id_row(80, "0123456789abcdef", "[]", &row);
  munit_assert_string_equal(row.text, "session.id: 0123456789abcdef []");
  for (int cols = 7; cols < 120; cols++) {
    tui_layout_session_id_row(cols, "0123456789abcdef0123456789abcdef", "[]", &row);
    munit_assert_int(row.x, >=, 1);
    munit_assert_int(row.x + row.width, ==, cols - 1);
    munit_assert_int(row.width, <=, 60);
    munit_assert_int(row.width, ==, text_columns(row.text, strlen(row.text)));
  }
  tui_layout_session_id_row(80, NULL, "[]", &row);
  munit_assert_int(row.width, ==, 0);
  tui_layout_session_row(80, NULL, "[]", &row);
  munit_assert_int(row.width, ==, 0);
  return MUNIT_OK;
}

static MunitResult test_status_row(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  setlocale(LC_CTYPE, "");
  TuiStatusRow row;
  tui_layout_status_row(100, "Thinking · 12s", "implement", "provider/model", "low", &row);
  munit_assert_string_equal(row.activity, "Thinking · 12s");
  munit_assert_string_equal(row.metadata, "implement · model · effort low");
  munit_assert_int(row.profile_width, ==, 9);
  tui_layout_status_row(40, "Thinking · 12s", "implement", "provider/model", "low", &row);
  munit_assert_string_equal(row.metadata, "implement · model");
  tui_layout_status_row(28, "Thinking · 12s", "implement", "provider/model", "low", &row);
  munit_assert_string_equal(row.metadata, "implement");
  for (int width = 0; width < 110; width++) {
    tui_layout_status_row(width, "Проверяю 目录 é · 12s", "implement", "目录/模型", "low", &row);
    int left = text_columns(row.activity, strlen(row.activity));
    int right = text_columns(row.metadata, strlen(row.metadata));
    munit_assert_int(left, <=, width);
    if (right) {
      munit_assert_int(row.metadata_x, >=, left + 3);
      munit_assert_int(row.metadata_x + right, ==, width);
    }
  }
  const char *fitting[] = {"é", "界́", "ab́"};
  for (size_t i = 0; i < sizeof(fitting) / sizeof(fitting[0]); i++) {
    int width = text_columns(fitting[i], strlen(fitting[i]));
    tui_layout_status_row(width, fitting[i], NULL, NULL, NULL, &row);
    munit_assert_string_equal(row.activity, fitting[i]);
  }
  tui_layout_status_row(2, "éxy", NULL, NULL, NULL, &row);
  munit_assert_string_equal(row.activity, "é…");
  tui_layout_status_row(80, "name\n\033", NULL, NULL, NULL, &row);
  munit_assert_string_equal(row.activity, "name  ");
  tui_layout_status_row(80, NULL, NULL, NULL, NULL, &row);
  munit_assert_string_equal(row.activity, "");
  munit_assert_string_equal(row.metadata, "");
  munit_assert_int(row.profile_width, ==, 0);
  tui_layout_status_row(80, NULL, "目录", "model", NULL, &row);
  munit_assert_int(row.profile_width, ==, 4);
  tui_layout_status_row(2, NULL, "implement", "model", NULL, &row);
  munit_assert_int(row.profile_width, ==, 0);
  tui_layout_status_row(80, NULL, NULL, "model", NULL, &row);
  munit_assert_int(row.profile_width, ==, 0);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/status_row", test_status_row, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/session_header", test_session_header, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tasks_viewport", test_tasks_viewport, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/workspace_footer", test_workspace_footer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/input_hit_area", test_input_hit_area, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite tui_layout_suite = {
    "/tui_layout", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
