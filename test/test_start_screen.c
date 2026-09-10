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
      munit_assert_int(content.shortcuts_y, ==, content.status_y + 5);
      munit_assert_int(content.shortcuts_y, ==,
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
  munit_assert_int(start_screen_wordmark_pixel(5, 10), ==, 1);
  /* Original 6x8 proportions retain two-row A crossbars. */
  for (int letter = 1; letter <= 5; letter += 4) {
    int x = letter * 8;
    int crossbar_rows = 0;
    for (int row = 2; row < START_SCREEN_WORDMARK_ROWS; row++) {
      munit_assert_int(start_screen_wordmark_pixel(row, x), ==, 1);
      munit_assert_int(start_screen_wordmark_pixel(row, x + 1), ==, 1);
      munit_assert_int(start_screen_wordmark_pixel(row, x + 4), ==, 1);
      munit_assert_int(start_screen_wordmark_pixel(row, x + 5), ==, 1);
      crossbar_rows += start_screen_wordmark_pixel(row, x + 2);
    }
    munit_assert_int(crossbar_rows, ==, 2);
    munit_assert_int(start_screen_wordmark_pixel(7, x + 2), ==, 0);
    munit_assert_int(start_screen_wordmark_pixel(7, x + 3), ==, 0);
  }
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

static MunitResult test_animation_opening(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  StartScreenAnimation animation = {0};
  long long opened = 1234567;
  int resting = start_screen_animation_frame(&animation, 1, opened);
  munit_assert_int(start_screen_animation_frame(&animation, 1, opened + 499), ==, resting);
  int first = start_screen_animation_frame(&animation, 1, opened + 500);
  munit_assert_int(first, ==, start_screen_animation_tick(0));
  munit_assert_int(resting, ==, 0);
  /* Redraws (including resize) keep the same opening timestamp. */
  munit_assert_int(start_screen_animation_frame(&animation, 1, opened + 1700), ==, 1200);
  munit_assert_int(start_screen_animation_frame(&animation, 1, opened + 500 + START_SCREEN_WAVE_PERIOD_MS), ==, first);
  start_screen_animation_frame(&animation, 0, opened + 4200);
  munit_assert_int(start_screen_animation_frame(&animation, 1, opened + 8000), ==, resting);
  munit_assert_int(start_screen_animation_frame(&animation, 1, opened + 8499), ==, resting);
  munit_assert_int(start_screen_animation_frame(&animation, 1, opened + 8500), ==, first);
  return MUNIT_OK;
}

static MunitResult test_wave_motion(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(START_SCREEN_WAVE_PERIOD_MS, ==, 2500);
  munit_assert_int(start_screen_animation_tick(2500), ==, 0);
  munit_assert_int(start_screen_animation_tick(-1), ==, 2499);
  munit_assert_int(start_screen_wave_level(4, 0, 0), ==, 6);
  munit_assert_int(start_screen_wave_level(4, 27, 0), ==, 1);
  /* Bezier easing: 16.875 px at 625 ms, 54 px at 1250 ms.
   * Equal time intervals cover increasing distances, unlike linear travel. */
  munit_assert_int(start_screen_wave_level(4, 17, 625), ==, 6);
  munit_assert_int(start_screen_wave_level(4, 27, 625), <, 6);
  munit_assert_int(start_screen_wave_level(4, 27, 816), ==, 6);
  munit_assert_int(start_screen_wave_level(4, 0, 816), ==, 1);
  munit_assert_int(start_screen_wave_level(4, 53, 1250), ==, 6);
  munit_assert_int(start_screen_wave_level(4, 0, 1250), ==, 1);
  /* No reverse pass through the center in the second half-cycle. */
  munit_assert_int(start_screen_wave_level(4, 27, 1875), ==, 1);
  for (int row = 0; row < START_SCREEN_WORDMARK_ROWS; row++) {
    for (int col = 0; col < START_SCREEN_WORDMARK_COLUMNS; col++) {
      if (!start_screen_wordmark_pixel(row, col)) continue;
      int low = 6, high = 1;
      for (int tick = 0; tick < START_SCREEN_WAVE_PERIOD_MS; tick += 20) {
        int level = start_screen_wave_level(row, col, tick);
        int next = start_screen_wave_level(row, col, tick + 20);
        munit_assert_int(abs(level - next), <=, 1);
        munit_assert_int(level, ==, start_screen_wave_level(row, col, tick + START_SCREEN_WAVE_PERIOD_MS));
        munit_assert_int(level, ==, start_screen_wave_level(row, col, tick - START_SCREEN_WAVE_PERIOD_MS));
        if (level < low) low = level;
        if (level > high) high = level;
      }
      munit_assert_int(low, ==, 1);
      munit_assert_int(high, ==, 6);
    }
  }
  return MUNIT_OK;
}

static MunitResult test_wave_mask_and_palette(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int seen[7] = {0};
  for (int tick = 0; tick < START_SCREEN_WAVE_PERIOD_MS; tick += 20) {
    for (int row = -1; row <= START_SCREEN_WORDMARK_ROWS; row++) {
      for (int col = -1; col <= START_SCREEN_WORDMARK_COLUMNS; col++) {
        int level = start_screen_wave_level(row, col, tick);
        munit_assert_int(level, >=, 0);
        munit_assert_int(level, <=, 6);
        seen[level]++;
        munit_assert_int(level != 0, ==, start_screen_wordmark_pixel(row, col));
        if (!level) continue;
        /* Adjacent stroke pixels cannot become isolated bright dots. */
        int right = start_screen_wave_level(row, col + 1, tick);
        int below = start_screen_wave_level(row + 1, col, tick);
        if (right) munit_assert_int(abs(level - right), <=, 1);
        if (below) munit_assert_int(abs(level - below), <=, 1);
        if (below && row % 2 == 0)
          munit_assert_int(level, ==, below);
      }
    }
  }
  for (int level = 0; level <= 6; level++) munit_assert_int(seen[level], >, 0);
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
  munit_assert_string_equal(lines.shortcuts,
                            "/ commands · Shift+Tab profiles");
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
    {"/animation_opening", test_animation_opening, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wave_motion",
     test_wave_motion, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wave_mask_and_palette", test_wave_mask_and_palette, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/build_status_values", test_build_status_values, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/build_status_fallbacks", test_build_status_fallbacks, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite start_screen_suite = {"/start_screen", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
