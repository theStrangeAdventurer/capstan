#include "munit.h"
#include "usage.h"
#include <string.h>

static MunitResult test_empty_usage_hidden(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32] = "x";
  UsageStats usage = {0, 0, 0, 0};
  int n = usage_format(usage, buf, sizeof(buf));
  munit_assert_int(n, ==, 0);
  munit_assert_string_equal(buf, "");
  return MUNIT_OK;
}

static MunitResult test_usage_format_small(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {123, 45, 168, 0};
  int n = usage_format(usage, buf, sizeof(buf));
  munit_assert_int(n, ==, (int)strlen("tok 123/45"));
  munit_assert_string_equal(buf, "tok 123/45");
  return MUNIT_OK;
}

static MunitResult test_usage_format_compact(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {1234, 12890, 14124, 0};
  usage_format(usage, buf, sizeof(buf));
  munit_assert_string_equal(buf, "tok 1.2k/12k");
  return MUNIT_OK;
}

static MunitResult test_usage_format_tiny_buffer(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[4];
  UsageStats usage = {123, 45, 168, 0};
  int n = usage_format(usage, buf, sizeof(buf));
  munit_assert_int(n, ==, 0);
  munit_assert_string_equal(buf, "tok");
  return MUNIT_OK;
}

static MunitResult test_usage_format_context_limit(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {1200, 800, 2000, 8000};
  int n = usage_format(usage, buf, sizeof(buf));
  munit_assert_int(n, ==, (int)strlen(" 1.2k/8.0k 15% "));
  munit_assert_string_equal(buf, " 1.2k/8.0k 15% ");
  return MUNIT_OK;
}

static MunitResult test_usage_format_context_limit_fallback_used(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {1200, 800, 0, 8000};
  usage_format(usage, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 1.2k/8.0k 15% ");
  return MUNIT_OK;
}

static MunitResult test_usage_format_context_limit_only(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {0, 0, 0, 8000};
  int n = usage_format(usage, buf, sizeof(buf));
  munit_assert_int(n, ==, (int)strlen(" 0/8.0k 0% "));
  munit_assert_string_equal(buf, " 0/8.0k 0% ");
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/empty_hidden", test_empty_usage_hidden, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/format_small", test_usage_format_small, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/format_compact", test_usage_format_compact, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/tiny_buffer", test_usage_format_tiny_buffer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/context_limit", test_usage_format_context_limit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/context_limit_fallback_used", test_usage_format_context_limit_fallback_used, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/context_limit_only", test_usage_format_context_limit_only, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite usage_suite = {"/usage", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
