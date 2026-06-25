#include "munit.h"
#include "scroll.h"

static MunitResult test_init(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_up(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_up(5);
  munit_assert_int(scroll_get(), ==, 5);
  munit_assert_int(scroll_is_following(), ==, 0);
  scroll_up(3);
  munit_assert_int(scroll_get(), ==, 8);
  return MUNIT_OK;
}

static MunitResult test_down_clamp(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_down(5);
  munit_assert_int(scroll_get(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_down_normal(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_up(10);
  scroll_down(3);
  munit_assert_int(scroll_get(), ==, 7);
  return MUNIT_OK;
}

static MunitResult test_down_to_zero(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_up(5);
  scroll_down(5);
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_down_overshoot(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_up(3);
  scroll_down(10);
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_set_positive(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_set(42);
  munit_assert_int(scroll_get(), ==, 42);
  munit_assert_int(scroll_is_following(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_set_negative_clamp(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_set(-1);
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_reset(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_up(100);
  scroll_reset();
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_follow_tail_keeps_bottom_on_growth(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_update_content(100, 10);
  scroll_update_content(125, 10);
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_manual_scroll_preserves_view_on_growth(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_update_content(100, 10);
  scroll_up(20);
  scroll_update_content(100, 10);
  munit_assert_int(scroll_get(), ==, 20);
  scroll_update_content(125, 10);
  munit_assert_int(scroll_get(), ==, 45);
  munit_assert_int(scroll_is_following(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_manual_scroll_reenables_follow_at_bottom(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  scroll_reset();
  scroll_update_content(100, 10);
  scroll_up(20);
  scroll_update_content(100, 10);
  scroll_down(20);
  scroll_update_content(125, 10);
  munit_assert_int(scroll_get(), ==, 0);
  munit_assert_int(scroll_is_following(), ==, 1);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/init", test_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/up", test_up, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/down_clamp", test_down_clamp, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/down_normal", test_down_normal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/down_to_zero", test_down_to_zero, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/down_overshoot", test_down_overshoot, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/set_positive", test_set_positive, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/set_negative_clamp", test_set_negative_clamp, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/reset", test_reset, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/follow_tail_keeps_bottom_on_growth", test_follow_tail_keeps_bottom_on_growth, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/manual_scroll_preserves_view_on_growth", test_manual_scroll_preserves_view_on_growth, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/manual_scroll_reenables_follow_at_bottom", test_manual_scroll_reenables_follow_at_bottom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite scroll_suite = {"/scroll", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
