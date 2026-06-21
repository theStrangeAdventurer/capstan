#include "munit.h"

extern MunitSuite http_stack_suite;
extern MunitSuite fetch_plugin_suite;
extern MunitSuite file_plugin_suite;
extern MunitSuite file_write_plugin_suite;
extern MunitSuite http_redirect_suite;
extern MunitSuite logs_plugin_suite;
extern MunitSuite provider_tools_suite;

int main(int argc, char *argv[]) {
  MunitSuite suites[] = {
    http_stack_suite,
    fetch_plugin_suite,
    file_plugin_suite,
    file_write_plugin_suite,
    http_redirect_suite,
    logs_plugin_suite,
    provider_tools_suite,
    {NULL, NULL, NULL, 0, 0}
  };
  MunitSuite suite = {
    "", NULL, suites, 1, MUNIT_SUITE_OPTION_NONE
  };
  return munit_suite_main(&suite, NULL, argc, argv);
}
