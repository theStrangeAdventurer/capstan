#include "munit.h"
#include "start_screen.h"
#include <stdlib.h>
#include <string.h>

static MunitResult test_layout_wide(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_layout_for_size(20, 64), ==,
                   START_SCREEN_WIDE);
  munit_assert_int(start_screen_layout_for_size(19, 64), ==,
                   START_SCREEN_COMPACT);
  return MUNIT_OK;
}

static MunitResult test_layout_compact(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_layout_for_size(12, 70), ==,
                   START_SCREEN_COMPACT);
  munit_assert_int(start_screen_layout_for_size(11, 70), ==,
                   START_SCREEN_MINIMAL);
  return MUNIT_OK;
}

static MunitResult test_layout_minimal(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_layout_for_size(5, 40), ==,
                   START_SCREEN_MINIMAL);
  return MUNIT_OK;
}

static MunitResult test_content_geometry(const MunitParameter params[],
                                         void *data) {
  (void)params;
  (void)data;
  for (int height = 1; height <= 40; height++) {
    for (int width = 1; width <= 160; width++) {
      StartScreenContent content = start_screen_content_for_size(height, width);
      StartScreenLayout layout = start_screen_layout_for_size(height, width);
      if (layout == START_SCREEN_MINIMAL) {
        munit_assert_int(content.width, ==, 0);
        munit_assert_int(content.height, ==, 0);
        continue;
      }
      munit_assert_int(content.width, <=, 56);
      munit_assert_int(content.width, >=, 44);
      munit_assert_int(content.x, ==, (width - content.width) / 2);
      munit_assert_int(content.y, ==, (height - content.height) / 2);
      munit_assert_int(content.x, >=, 2);
      munit_assert_int(content.y, >=, 0);
      munit_assert_int(content.x + content.width, <=, width - 2);
      munit_assert_int(content.y + content.height, <=, height);
      munit_assert_int(content.status_y, ==, content.y +
                       (layout == START_SCREEN_WIDE ? 8 : 3));
      if (layout == START_SCREEN_WIDE) {
        munit_assert_int(content.version_y, ==,
                         content.y + START_SCREEN_WORDMARK_DISPLAY_ROWS + 2);
        munit_assert_int(content.status_y, ==, content.version_y + 2);
      } else {
        munit_assert_int(content.version_y, ==, content.y);
      }
      munit_assert_int(content.ready_y, ==, content.status_y + 5);
      munit_assert_int(content.ready_y + 1, ==,
                       content.y + content.height - 1);
      if (layout == START_SCREEN_WIDE)
        munit_assert_int(content.width, >=, START_SCREEN_WORDMARK_COLUMNS);
    }
  }
  return MUNIT_OK;
}

static MunitResult test_collapse_home_child(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;
  munit_assert_int(setenv("HOME", "/home/me", 1), ==, 0);
  char out[64];
  start_screen_collapse_home("/home/me/project/repo", out, sizeof(out));
  munit_assert_string_equal(out, "~/project/repo");
  return MUNIT_OK;
}

static MunitResult test_collapse_home_exact(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;
  munit_assert_int(setenv("HOME", "/home/me", 1), ==, 0);
  char out[64];
  start_screen_collapse_home("/home/me", out, sizeof(out));
  munit_assert_string_equal(out, "~");
  return MUNIT_OK;
}

static MunitResult test_collapse_home_other_path(const MunitParameter params[],
                                                void *data) {
  (void)params;
  (void)data;
  munit_assert_int(setenv("HOME", "/home/me", 1), ==, 0);
  char out[64];
  start_screen_collapse_home("/work/repo", out, sizeof(out));
  munit_assert_string_equal(out, "/work/repo");
  return MUNIT_OK;
}

static MunitResult test_truncate_ascii(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  char out[32];
  start_screen_truncate("abcdefghijklmnopqrstuvwxyz", out, sizeof(out), 10);
  munit_assert_string_equal(out, "abcdefg...");
  return MUNIT_OK;
}

static MunitResult test_truncate_utf8_boundary(const MunitParameter params[],
                                               void *data) {
  (void)params;
  (void)data;
  char out[32];
  start_screen_truncate("привет-мир", out, sizeof(out), 6);
  munit_assert_string_equal(out, "при...");
  return MUNIT_OK;
}

static MunitResult test_wordmark_shape(const MunitParameter params[],
                                        void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_wordmark_pixel(0, 1), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(0, 0), ==, 0);
  munit_assert_int(START_SCREEN_WORDMARK_COLUMNS, ==, 54);
  munit_assert_int(START_SCREEN_WORDMARK_DISPLAY_ROWS, ==, 4);
  /* A has two-pixel stems, an open counter, and a solid crossbar. */
  munit_assert_int(start_screen_wordmark_pixel(3, 8), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(3, 9), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(3, 10), ==, 0);
  munit_assert_int(start_screen_wordmark_pixel(3, 11), ==, 0);
  munit_assert_int(start_screen_wordmark_pixel(3, 12), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(3, 13), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(4, 10), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(START_SCREEN_WORDMARK_ROWS, 0),
                   ==, 0);
  for (int letter = 0; letter < START_SCREEN_WORDMARK_LETTERS - 1; letter++) {
    int gap = letter * (START_SCREEN_WORDMARK_LETTER_COLUMNS +
                        START_SCREEN_WORDMARK_LETTER_GAP) +
              START_SCREEN_WORDMARK_LETTER_COLUMNS;
    for (int row = 0; row < START_SCREEN_WORDMARK_ROWS; row++) {
      for (int column = gap; column < gap + START_SCREEN_WORDMARK_LETTER_GAP;
           column++)
        munit_assert_int(start_screen_wordmark_pixel(row, column), ==, 0);
    }
  }
  munit_assert_int(start_screen_wordmark_pixel(-1, 0), ==, 0);
  munit_assert_int(start_screen_wordmark_pixel(0, START_SCREEN_WORDMARK_COLUMNS),
                   ==, 0);
  return MUNIT_OK;
}

static MunitResult test_wordmark_half_cells(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_wordmark_cell(0, 0), ==, 2);
  munit_assert_int(start_screen_wordmark_cell(3, 0), ==, 1);
  munit_assert_int(start_screen_wordmark_cell(0, 1), ==, 3);
  munit_assert_int(start_screen_wordmark_cell(1, 2), ==, 0);
  munit_assert_int(start_screen_wordmark_cell(-1, 0), ==, 0);
  munit_assert_int(start_screen_wordmark_cell(
                       START_SCREEN_WORDMARK_DISPLAY_ROWS, 0), ==, 0);
  munit_assert_int(start_screen_wordmark_cell(0, -1), ==, 0);
  munit_assert_int(start_screen_wordmark_cell(
                       0, START_SCREEN_WORDMARK_COLUMNS), ==, 0);
  for (int row = 0; row < START_SCREEN_WORDMARK_DISPLAY_ROWS; row++) {
    for (int column = 0; column < START_SCREEN_WORDMARK_COLUMNS; column++) {
      int cell = start_screen_wordmark_cell(row, column);
      munit_assert_int(cell & 1, ==,
                       start_screen_wordmark_pixel(row * 2, column));
      munit_assert_int((cell >> 1) & 1, ==,
                       start_screen_wordmark_pixel(row * 2 + 1, column));
    }
  }
  return MUNIT_OK;
}

static MunitResult test_animation_accelerates_then_pauses(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int first_step = start_screen_animation_tick(300) -
                   start_screen_animation_tick(0);
  int second_step = start_screen_animation_tick(600) -
                    start_screen_animation_tick(300);
  int third_step = start_screen_animation_tick(899) -
                   start_screen_animation_tick(600);
  munit_assert_int(first_step, <, second_step);
  munit_assert_int(second_step, <, third_step);
  munit_assert_int(start_screen_animation_tick(900), ==,
                   start_screen_animation_tick(1200));
  munit_assert_int(start_screen_animation_tick(900), ==,
                   start_screen_animation_tick(3599));
  munit_assert_int(start_screen_animation_tick(3600), ==,
                   start_screen_animation_tick(0));
  munit_assert_int(start_screen_animation_tick(-1), ==,
                   start_screen_animation_tick(3599));
  /* The longer pause must leave every pixel at its resting color. */
  for (int row = 0; row < START_SCREEN_WORDMARK_ROWS; row++) {
    for (int column = 0; column < START_SCREEN_WORDMARK_COLUMNS; column++)
      munit_assert_int(start_screen_gradient_level(
                           row, column, start_screen_animation_tick(3599)),
                       ==, 1);
  }
  return MUNIT_OK;
}

static MunitResult test_gradient_sweeps_diagonally(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_gradient_level(0, 12, 26), ==, 6);
  munit_assert_int(start_screen_gradient_level(1, 11, 26), ==, 6);
  munit_assert_int(start_screen_gradient_level(0, 11, 26), ==, 5);
  munit_assert_int(start_screen_gradient_level(0, 10, 26), ==, 4);
  munit_assert_int(start_screen_gradient_level(0, 9, 26), ==, 4);
  munit_assert_int(start_screen_gradient_level(0, 8, 26), ==, 3);
  munit_assert_int(start_screen_gradient_level(0, 6, 26), ==, 2);
  munit_assert_int(start_screen_gradient_level(0, 4, 26), ==, 1);
  munit_assert_int(start_screen_gradient_level(0, 12, 28), !=,
                   start_screen_gradient_level(0, 12, 26));
  munit_assert_int(start_screen_gradient_level(-1, 0, 0), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_build_status_values(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;
  munit_assert_int(setenv("HOME", "/Users/alxd", 1), ==, 0);
  StartScreenStatus status = {
      .provider = "openrouter",
      .model = "deepseek/deepseek-v4-pro",
      .reasoning_effort = "high",
      .profile = "plan",
      .workdir = "/Users/alxd/narnia/tui-agent",
  };
  StartScreenStatusLines lines;
  start_screen_build_status(&status, &lines);

  munit_assert_string_equal(lines.model,
                            "openrouter/deepseek/deepseek-...");
  munit_assert_string_equal(lines.reasoning_effort, "high");
  munit_assert_string_equal(lines.profile, "plan");
  munit_assert_string_equal(lines.workdir, "~/narnia/tui-agent");
  munit_assert_string_equal(lines.ready, "Type a message to begin");
  munit_assert_string_equal(lines.shortcuts,
                            "/models choose model · Shift+Tab profiles");
  return MUNIT_OK;
}

static MunitResult test_build_status_fallbacks(const MunitParameter params[],
                                               void *data) {
  (void)params;
  (void)data;
  StartScreenStatus status = {0};
  StartScreenStatusLines lines;
  start_screen_build_status(&status, &lines);

  munit_assert_string_equal(lines.model, "not configured");
  munit_assert_string_equal(lines.reasoning_effort, "default");
  munit_assert_string_equal(lines.profile, "implement");
  munit_assert_string_equal(lines.workdir, ".");
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/layout_wide", test_layout_wide, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/layout_compact", test_layout_compact, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/layout_minimal", test_layout_minimal, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/content_geometry", test_content_geometry, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/collapse_home_child", test_collapse_home_child, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/collapse_home_exact", test_collapse_home_exact, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/collapse_home_other_path", test_collapse_home_other_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/truncate_ascii", test_truncate_ascii, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/truncate_utf8_boundary", test_truncate_utf8_boundary, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wordmark_shape", test_wordmark_shape, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wordmark_half_cells",
     test_wordmark_half_cells, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/animation_accelerates_then_pauses",
     test_animation_accelerates_then_pauses, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/gradient_sweeps_diagonally", test_gradient_sweeps_diagonally, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/build_status_values", test_build_status_values, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/build_status_fallbacks", test_build_status_fallbacks, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite start_screen_suite = {"/start_screen", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
