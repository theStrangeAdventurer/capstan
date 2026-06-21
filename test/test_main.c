#include "munit.h"

extern MunitSuite dispatch_suite;
extern MunitSuite input_suite;
extern MunitSuite linemap_suite;
extern MunitSuite mode_suite;
extern MunitSuite permit_logic_suite;
extern MunitSuite permit_prompt_suite;
extern MunitSuite popup_suite;
extern MunitSuite scroll_suite;
extern MunitSuite skills_suite;
extern MunitSuite usage_suite;
extern MunitSuite utils_suite;
extern MunitSuite visual_suite;

int main(int argc, char *argv[]) {
  MunitSuite suites[] = {
    dispatch_suite,
    input_suite,
    linemap_suite,
    mode_suite,
    permit_logic_suite,
    permit_prompt_suite,
    popup_suite,
    scroll_suite,
    skills_suite,
    usage_suite,
    utils_suite,
    visual_suite,
    {NULL, NULL, NULL, 0, 0}
  };
  MunitSuite suite = {
    "", NULL, suites, 1, MUNIT_SUITE_OPTION_NONE
  };
  return munit_suite_main(&suite, NULL, argc, argv);
}
