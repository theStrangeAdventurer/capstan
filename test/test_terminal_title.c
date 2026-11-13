#include "terminal_title.h"
#include "munit.h"
#include <locale.h>
#include <string.h>

static MunitResult test_sanitize(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  setlocale(LC_CTYPE, "C");
  char out[256];
  terminal_title_sanitize("Сессия — fix", out, sizeof(out));
  munit_assert_string_equal(out, "Сессия — fix");
  terminal_title_sanitize("a\033]2;bad\007\n\r\t\177\302\235\302\234z", out, sizeof(out));
  munit_assert_string_equal(out, "a]2;badz");
  terminal_title_sanitize(NULL, out, sizeof(out));
  munit_assert_string_equal(out, "");
  terminal_title_sanitize("ЯЯ", out, 4);
  munit_assert_string_equal(out, "Я");
  terminal_title_sanitize("x", out, 1);
  munit_assert_string_equal(out, "");
  terminal_title_sanitize("x", NULL, 0);
  terminal_title_sanitize("\377ok", out, sizeof(out));
  munit_assert_string_equal(out, "ok");
  terminal_title_sanitize("\300\257\340\200\257\355\240\200\364\220\200\200ok\342\202", out, sizeof(out));
  munit_assert_string_equal(out, "ok");
  terminal_title_sanitize("😀", out, sizeof(out));
  munit_assert_string_equal(out, "😀");
  setlocale(LC_CTYPE, "");
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/sanitize", test_sanitize, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite terminal_title_suite = {"/terminal_title", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
