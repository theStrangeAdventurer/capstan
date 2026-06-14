#include "munit.h"

extern MunitSuite input_suite;
extern MunitSuite scroll_suite;
extern MunitSuite utils_suite;

int main(int argc, char *argv[]) {
  MunitSuite suites[] = {
    input_suite,
    scroll_suite,
    utils_suite,
    {NULL, NULL, NULL, 0, 0}
  };
  MunitSuite suite = {
    "", NULL, suites, 1, MUNIT_SUITE_OPTION_NONE
  };
  return munit_suite_main(&suite, NULL, argc, argv);
}
