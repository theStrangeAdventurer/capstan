#include "munit.h"

extern MunitSuite dispatch_suite;
extern MunitSuite app_config_suite;
extern MunitSuite cli_args_suite;
extern MunitSuite clipboard_suite;
extern MunitSuite diff_highlight_suite;
extern MunitSuite finder_suite;
extern MunitSuite input_suite;
extern MunitSuite input_history_suite;
extern MunitSuite linemap_suite;
extern MunitSuite mode_suite;
extern MunitSuite permit_logic_suite;
extern MunitSuite permit_prompt_suite;
extern MunitSuite popup_suite;
extern MunitSuite redact_suite;
extern MunitSuite scroll_suite;
extern MunitSuite session_suite;
extern MunitSuite shell_process_suite;
extern MunitSuite start_screen_suite;
extern MunitSuite skills_suite;
extern MunitSuite submission_queue_suite;
extern MunitSuite usage_suite;
extern MunitSuite utils_suite;
extern MunitSuite visual_suite;
extern MunitSuite wiki_suite;

int main(int argc, char *argv[]) {
  MunitSuite suites[] = {
    dispatch_suite,
    app_config_suite,
    cli_args_suite,
    clipboard_suite,
    diff_highlight_suite,
    finder_suite,
    input_suite,
    input_history_suite,
    linemap_suite,
    mode_suite,
    permit_logic_suite,
    permit_prompt_suite,
    popup_suite,
    redact_suite,
    scroll_suite,
    session_suite,
    shell_process_suite,
    start_screen_suite,
    skills_suite,
    submission_queue_suite,
    usage_suite,
    utils_suite,
    visual_suite,
    wiki_suite,
    {NULL, NULL, NULL, 0, 0}
  };
  MunitSuite suite = {
    "", NULL, suites, 1, MUNIT_SUITE_OPTION_NONE
  };
  return munit_suite_main(&suite, NULL, argc, argv);
}
