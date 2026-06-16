#include "munit.h"

extern MunitSuite http_stack_suite;

int main(int argc, char *argv[]) {
  MunitSuite suites[] = {
    http_stack_suite,
    {NULL, NULL, NULL, 0, 0}
  };
  MunitSuite suite = {
    "", NULL, suites, 1, MUNIT_SUITE_OPTION_NONE
  };
  return munit_suite_main(&suite, NULL, argc, argv);
}
