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
  munit_assert_int(n, ==, (int)strlen("tok in 123 out 45"));
  munit_assert_string_equal(buf, "tok in 123 out 45");
  return MUNIT_OK;
}

static MunitResult test_usage_format_compact(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {1234, 12890, 14124, 0};
  usage_format(usage, buf, sizeof(buf));
  munit_assert_string_equal(buf, "tok in 1.2k out 12.9k");
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
  munit_assert_int(n, ==, (int)strlen(" 2.0k/8.0k 25% "));
  munit_assert_string_equal(buf, " 2.0k/8.0k 25% ");
  return MUNIT_OK;
}

static MunitResult test_usage_format_context_limit_fallback_used(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[32];
  UsageStats usage = {1200, 800, 0, 8000};
  usage_format(usage, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 2.0k/8.0k 25% ");
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

static MunitResult test_large_context(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[64];
  UsageStats usage = {30000000, 1000, 30001000, 60000000};
  usage_format(usage, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 30.0m/60.0m 50% ");
  usage = (UsageStats){2147483647, 0, 2147483647, 1};
  usage_format(usage, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 2147.5m/1 214748364700% ");
  return MUNIT_OK;
}

static MunitResult test_context_growth(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[64];
  usage_format((UsageStats){38000, 0, 38000, 100000}, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 38.0k/100.0k 38% ");
  /* The current response occupies context immediately. */
  usage_format((UsageStats){38000, 600, 38600, 100000}, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 38.6k/100.0k 39% ");
  /* Next input contains the old response, not another 38000 tokens. */
  usage_format((UsageStats){38600, 400, 39000, 100000}, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 39.0k/100.0k 39% ");
  /* An explicit provider total is authoritative. */
  usage_format((UsageStats){38600, 400, 38000, 100000}, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 38.0k/100.0k 38% ");
  /* Compaction really releases context. */
  usage_format((UsageStats){1000, 0, 1000, 100000}, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 1.0k/100.0k 1% ");
  usage_format((UsageStats){2147483647, 2147483647, 0, 1}, buf, sizeof(buf));
  munit_assert_string_equal(buf, " 2147.5m/1 214748364700% ");
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/context_growth", test_context_growth, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/large_context", test_large_context, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
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
