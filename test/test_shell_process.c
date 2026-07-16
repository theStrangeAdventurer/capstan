#include "munit.h"
#include "shell_process.h"
#include <sys/time.h>

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static MunitResult test_timeout_kills_pipeline_process_group(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  ShellProcessResult result;
  long long started = now_ms();
  munit_assert_true(shell_process_run("sleep 30 | cat", "/tmp", 1, 1024,
                                      1024, NULL, &result));
  long long elapsed = now_ms() - started;
  munit_assert_true(result.timed_out);
  munit_assert_int((int)elapsed, <, 4000);
  shell_process_result_free(&result);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/timeout_kills_pipeline_process_group",
     test_timeout_kills_pipeline_process_group, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite shell_process_suite = {"/shell_process", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
