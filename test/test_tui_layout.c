#include "munit.h"
#include "tui_layout.h"

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

static MunitTest tests[] = {
    {"/input_hit_area", test_input_hit_area, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite tui_layout_suite = {
    "/tui_layout", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
