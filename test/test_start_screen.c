#include "munit.h"
#include "start_screen.h"
#include <stdlib.h>
#include <string.h>

static MunitResult test_layout_wide(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_layout_for_size(15, 81), ==,
                   START_SCREEN_WIDE);
  return MUNIT_OK;
}

static MunitResult test_layout_compact(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  munit_assert_int(start_screen_layout_for_size(14, 70), ==,
                   START_SCREEN_COMPACT);
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
                            "type your question or / + Tab for options");
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
    {"/build_status_values", test_build_status_values, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/build_status_fallbacks", test_build_status_fallbacks, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite start_screen_suite = {"/start_screen", tests, NULL, 1,
                                MUNIT_SUITE_OPTION_NONE};
