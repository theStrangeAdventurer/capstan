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
  munit_assert_int(start_screen_wordmark_pixel(3, 8), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(3, 9), ==, 1);
  munit_assert_int(start_screen_wordmark_pixel(-1, 0), ==, 0);
  munit_assert_int(start_screen_wordmark_pixel(0, START_SCREEN_WORDMARK_COLUMNS),
                   ==, 0);
  return MUNIT_OK;
}

static MunitResult test_wordmark_grain_is_sparse_and_fixed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_wordmark_grain(0, 11), ==, 1);
  munit_assert_int(start_screen_wordmark_grain(0, 12), ==, 0);
  munit_assert_int(start_screen_wordmark_grain(0, 0), ==, 0);
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
  munit_assert_int(start_screen_animation_tick(1350), ==,
                   start_screen_animation_tick(0));
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
  munit_assert_string_equal(lines.ready,
                            "Type message · / + Tab for commands · Shift+Tab: switch profile");
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
    {"/wordmark_grain_is_sparse_and_fixed",
     test_wordmark_grain_is_sparse_and_fixed, NULL, NULL,
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
