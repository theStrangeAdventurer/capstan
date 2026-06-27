#include "munit.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

static MunitResult test_my_strdup(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *s = "hello";
  char *dup = my_strdup(s);
  munit_assert_string_equal(dup, "hello");
  munit_assert_ptr_not_equal(dup, s);
  free(dup);
  return MUNIT_OK;
}

static MunitResult test_my_strdup_empty(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *dup = my_strdup("");
  munit_assert_string_equal(dup, "");
  free(dup);
  return MUNIT_OK;
}

static MunitResult test_replace_with_found(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[64];
  strcpy(buf, "/file README.md");
  int r = replace_with(buf, sizeof(buf), "/file", "/read");
  munit_assert_int(r, ==, 1);
  munit_assert_string_equal(buf, "/read README.md");
  return MUNIT_OK;
}

static MunitResult test_replace_with_not_found(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[64];
  strcpy(buf, "hello world");
  int r = replace_with(buf, sizeof(buf), "xyz", "abc");
  munit_assert_int(r, ==, 0);
  munit_assert_string_equal(buf, "hello world");
  return MUNIT_OK;
}

static MunitResult test_replace_with_invalid(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int r = replace_with(NULL, 10, "a", "b");
  munit_assert_int(r, ==, -1);
  return MUNIT_OK;
}

static MunitResult test_replace_with_overflow_rejected(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[8];
  strcpy(buf, "a.txt");
  int r = replace_with(buf, sizeof(buf), "a", "very-long");
  munit_assert_int(r, ==, -1);
  munit_assert_string_equal(buf, "a.txt");
  return MUNIT_OK;
}

static MunitResult test_replace_with_requires_terminated_input(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char buf[4] = {'a', 'b', 'c', 'd'};
  int r = replace_with(buf, sizeof(buf), "a", "z");
  munit_assert_int(r, ==, -1);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/my_strdup", test_my_strdup, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/my_strdup_empty", test_my_strdup_empty, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/replace_found", test_replace_with_found, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/replace_not_found", test_replace_with_not_found, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/replace_invalid", test_replace_with_invalid, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/replace_overflow_rejected", test_replace_with_overflow_rejected, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/replace_requires_terminated_input",
   test_replace_with_requires_terminated_input, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite utils_suite = {"/utils", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
