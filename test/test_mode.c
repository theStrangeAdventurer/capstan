#include "munit.h"
#include "mode.h"

static MunitResult test_init(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    munit_assert_int(mode_get(), ==, FOCUS_INPUT);
    return MUNIT_OK;
}

static MunitResult test_set_input(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    mode_set(FOCUS_MESSAGES);
    munit_assert_int(mode_get(), ==, FOCUS_MESSAGES);
    mode_set(FOCUS_INPUT);
    munit_assert_int(mode_get(), ==, FOCUS_INPUT);
    return MUNIT_OK;
}

static MunitResult test_set_messages(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    mode_set(FOCUS_MESSAGES);
    munit_assert_int(mode_get(), ==, FOCUS_MESSAGES);
    return MUNIT_OK;
}

static MunitResult test_toggle(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    mode_set(FOCUS_INPUT);
    mode_toggle();
    munit_assert_int(mode_get(), ==, FOCUS_MESSAGES);
    return MUNIT_OK;
}

static MunitResult test_toggle_back(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    mode_set(FOCUS_INPUT);
    mode_toggle();
    mode_toggle();
    munit_assert_int(mode_get(), ==, FOCUS_INPUT);
    return MUNIT_OK;
}

static MunitResult test_focus_toggle_key(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    munit_assert_int(mode_is_focus_toggle_key(APP_KEY_CTRL_F), ==, 1);
    munit_assert_int(mode_is_focus_toggle_key(APP_KEY_ESCAPE), ==, 0);
    munit_assert_int(mode_is_focus_toggle_key(APP_KEY_SHIFT_TAB), ==, 0);
    munit_assert_int(mode_is_focus_toggle_key('\t'), ==, 0);
    return MUNIT_OK;
}

static MunitResult test_profile_cycle_key(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    munit_assert_int(mode_is_profile_cycle_key(APP_KEY_SHIFT_TAB), ==, 1);
    munit_assert_int(mode_is_profile_cycle_key(APP_KEY_CTRL_F), ==, 0);
    munit_assert_int(mode_is_profile_cycle_key(APP_KEY_ESCAPE), ==, 0);
    munit_assert_int(mode_is_profile_cycle_key('\t'), ==, 0);
    return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/init", test_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/set_input", test_set_input, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/set_messages", test_set_messages, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/toggle", test_toggle, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/toggle_back", test_toggle_back, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/focus_toggle_key", test_focus_toggle_key, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/profile_cycle_key", test_profile_cycle_key, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite mode_suite = {"/mode", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
