#include "cli_args.h"
#include "munit.h"

static MunitResult test_default_tui(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan"};
  CliOptions opts = cli_parse(1, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_TUI);
  munit_assert_int(opts.max_turns, ==, 200);
  return MUNIT_OK;
}

static MunitResult test_run_options(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--prompt", "hello", "--provider", "p",
                  "--model", "m", "--workdir", "/tmp", "--max-turns", "7",
                  "--json"};
  CliOptions opts = cli_parse(13, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_RUN);
  munit_assert_string_equal(opts.prompt, "hello");
  munit_assert_string_equal(opts.provider, "p");
  munit_assert_string_equal(opts.model, "m");
  munit_assert_string_equal(opts.workdir, "/tmp");
  munit_assert_int(opts.max_turns, ==, 7);
  munit_assert_int(opts.json, ==, 1);
  return MUNIT_OK;
}

static MunitResult test_prompt_conflict(const MunitParameter params[],
                                        void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--prompt", "a", "--prompt-file", "b"};
  CliOptions opts = cli_parse(6, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ERROR);
  munit_assert_not_null(opts.error);
  return MUNIT_OK;
}

static MunitResult test_self_test_mode(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "--self-test-embedded"};
  CliOptions opts = cli_parse(2, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_SELF_TEST);
  return MUNIT_OK;
}

static MunitResult test_max_turns_rejects_overflow(const MunitParameter params[],
                                                   void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--max-turns", "999999999999999999999"};
  CliOptions opts = cli_parse(4, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ERROR);
  munit_assert_not_null(opts.error);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/default_tui", test_default_tui, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/run_options", test_run_options, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/prompt_conflict", test_prompt_conflict, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/self_test_mode", test_self_test_mode, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/max_turns_rejects_overflow", test_max_turns_rejects_overflow, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite cli_args_suite = {"/cli_args", tests, NULL, 1,
                             MUNIT_SUITE_OPTION_NONE};
