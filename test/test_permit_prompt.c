#include "munit.h"
#include "permit_prompt.h"
#include "popup_internal.h"

static MunitResult test_navigation(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int choice = PERMIT_CHOICE_ONCE;

  munit_assert_int(permit_prompt_handle_key('j', &choice), ==,
                   PERMIT_PROMPT_CONTINUE);
  munit_assert_int(choice, ==, PERMIT_CHOICE_ALWAYS);
  munit_assert_int(permit_prompt_handle_key(POPUP_KEY_DOWN, &choice), ==,
                   PERMIT_PROMPT_CONTINUE);
  munit_assert_int(choice, ==, PERMIT_CHOICE_TOOL);
  munit_assert_int(permit_prompt_handle_key('k', &choice), ==,
                   PERMIT_PROMPT_CONTINUE);
  munit_assert_int(choice, ==, PERMIT_CHOICE_ALWAYS);

  return MUNIT_OK;
}

static MunitResult test_enter_confirms_current_choice(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int choice = PERMIT_CHOICE_ALWAYS;

  munit_assert_int(permit_prompt_handle_key('\n', &choice), ==,
                   PERMIT_PROMPT_DONE);
  munit_assert_string_equal(permit_prompt_result(choice), "allow_session");

  return MUNIT_OK;
}

static MunitResult test_shortcuts_confirm_explicit_choices(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int choice = PERMIT_CHOICE_REJECT;

  munit_assert_int(permit_prompt_handle_key('y', &choice), ==,
                   PERMIT_PROMPT_DONE);
  munit_assert_string_equal(permit_prompt_result(choice), "allow");

  choice = PERMIT_CHOICE_ONCE;
  munit_assert_int(permit_prompt_handle_key('a', &choice), ==,
                   PERMIT_PROMPT_DONE);
  munit_assert_string_equal(permit_prompt_result(choice), "allow_session");

  choice = PERMIT_CHOICE_ONCE;
  munit_assert_int(permit_prompt_handle_key('t', &choice), ==,
                   PERMIT_PROMPT_DONE);
  munit_assert_string_equal(permit_prompt_result(choice), "allow_tool_run");

  choice = PERMIT_CHOICE_ONCE;
  munit_assert_int(permit_prompt_handle_key('n', &choice), ==,
                   PERMIT_PROMPT_DONE);
  munit_assert_string_equal(permit_prompt_result(choice), "deny");

  return MUNIT_OK;
}

static MunitResult test_escape_denies(const MunitParameter params[],
                                      void *data) {
  (void)params;
  (void)data;
  int choice = PERMIT_CHOICE_ALWAYS;

  munit_assert_int(permit_prompt_handle_key(27, &choice), ==,
                   PERMIT_PROMPT_DONE);
  munit_assert_string_equal(permit_prompt_result(choice), "deny");

  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/navigation", test_navigation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/enter_confirms_current_choice", test_enter_confirms_current_choice, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/shortcuts_confirm_explicit_choices",
     test_shortcuts_confirm_explicit_choices, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/escape_denies", test_escape_denies, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite permit_prompt_suite = {"/permit_prompt", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
