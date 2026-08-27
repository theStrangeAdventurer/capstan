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

static MunitResult test_tui_yolo(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "--yolo"};
  CliOptions opts = cli_parse(2, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_TUI);
  munit_assert_int(opts.yolo, ==, 1);
  return MUNIT_OK;
}

static MunitResult test_tui_combines_runtime_options(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "--yolo", "--provider", "deepseek",
                  "--model", "deepseek-chat", "--profile", "plan",
                  "--effort", "high", "--workdir", "/tmp",
                  "--workspace", "/", "--max-turns", "7", "--no-mcp",
                  "--no-wiki", "--no-preserve-reasoning"};
  CliOptions opts = cli_parse(19, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_TUI);
  munit_assert_int(opts.yolo, ==, 1);
  munit_assert_string_equal(opts.provider, "deepseek");
  munit_assert_string_equal(opts.model, "deepseek-chat");
  munit_assert_string_equal(opts.profile, "plan");
  munit_assert_string_equal(opts.reasoning_effort, "high");
  munit_assert_string_equal(opts.workdir, "/tmp");
  munit_assert_string_equal(opts.workspace, "/");
  munit_assert_int(opts.max_turns, ==, 7);
  munit_assert_int(opts.max_turns_set, ==, 1);
  munit_assert_int(opts.no_mcp, ==, 1);
  munit_assert_int(opts.no_wiki, ==, 1);
  munit_assert_int(opts.no_preserve_reasoning, ==, 1);
  return MUNIT_OK;
}

static MunitResult test_tui_session_option(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "--session-id", "named session"};
  CliOptions opts = cli_parse(3, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_TUI);
  munit_assert_string_equal(opts.session_id, "named session");
  return MUNIT_OK;
}

static MunitResult test_rejects_removed_set_session_id(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "--set-session-id", "new"};
  CliOptions opts = cli_parse(3, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ERROR);
  munit_assert_not_null(opts.error);
  return MUNIT_OK;
}

static MunitResult test_tui_rejects_headless_options(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "--json"};
  CliOptions opts = cli_parse(2, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ERROR);
  munit_assert_not_null(opts.error);
  return MUNIT_OK;
}

static MunitResult test_run_options(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan",   "run",     "--prompt", "hello", "--provider",
                  "p",         "--model", "m",        "--reasoning-effort",
                  "high",      "--workdir", "/tmp",   "--workspace",
                  "/",         "--max-turns", "7",    "--profile",
                  "implement", "--session-id", "my fucking bench", "--json"};
  CliOptions opts = cli_parse(21, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_RUN);
  munit_assert_string_equal(opts.prompt, "hello");
  munit_assert_string_equal(opts.provider, "p");
  munit_assert_string_equal(opts.model, "m");
  munit_assert_string_equal(opts.profile, "implement");
  munit_assert_string_equal(opts.reasoning_effort, "high");
  munit_assert_string_equal(opts.workdir, "/tmp");
  munit_assert_string_equal(opts.workspace, "/");
  munit_assert_string_equal(opts.session_id, "my fucking bench");
  munit_assert_int(opts.max_turns, ==, 7);
  munit_assert_int(opts.json, ==, 1);
  return MUNIT_OK;
}

static MunitResult test_session_id_requires_value(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--session-id"};
  CliOptions opts = cli_parse(3, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ERROR);
  munit_assert_not_null(opts.error);
  return MUNIT_OK;
}

static MunitResult test_effort_alias(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--prompt", "hello", "--effort", "low"};
  CliOptions opts = cli_parse(6, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_RUN);
  munit_assert_string_equal(opts.reasoning_effort, "low");
  return MUNIT_OK;
}

static MunitResult test_profile_accepts_custom_name(const MunitParameter params[],
                                                     void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--profile", "architect"};
  CliOptions opts = cli_parse(4, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_RUN);
  munit_assert_string_equal(opts.profile, "architect");
  return MUNIT_OK;
}

static MunitResult test_reasoning_effort_rejects_unknown(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--reasoning-effort", "architect"};
  CliOptions opts = cli_parse(4, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ERROR);
  munit_assert_not_null(opts.error);
  return MUNIT_OK;
}

static MunitResult test_benchmark_sets_presets(const MunitParameter params[],
                                               void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run", "--prompt", "hello", "--benchmark"};
  CliOptions opts = cli_parse(5, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_RUN);
  munit_assert_int(opts.benchmark, ==, 1);
  munit_assert_int(opts.no_mcp, ==, 1);
  munit_assert_int(opts.no_wiki, ==, 1);
  munit_assert_int(opts.yolo, ==, 0);
  return MUNIT_OK;
}

static MunitResult test_explicit_headless_controls(const MunitParameter params[],
                                                   void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "run",      "--prompt", "hello",
                  "--no-mcp", "--no-wiki", "--no-preserve-reasoning",
                  "--yolo"};
  CliOptions opts = cli_parse(8, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_RUN);
  munit_assert_int(opts.no_mcp, ==, 1);
  munit_assert_int(opts.no_wiki, ==, 1);
  munit_assert_int(opts.no_preserve_reasoning, ==, 1);
  munit_assert_int(opts.yolo, ==, 1);
  munit_assert_int(opts.benchmark, ==, 0);
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

static MunitResult test_acp_mode(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "acp"};
  CliOptions opts = cli_parse(2, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ACP);
  return MUNIT_OK;
}

static MunitResult test_acp_yolo(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "acp", "--yolo"};
  CliOptions opts = cli_parse(3, argv);
  munit_assert_int(opts.mode, ==, CLI_MODE_ACP);
  munit_assert_int(opts.yolo, ==, 1);
  return MUNIT_OK;
}

static MunitResult test_acp_rejects_other_options(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *argv[] = {"capstan", "acp", "--json"};
  CliOptions opts = cli_parse(3, argv);
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
    {"/tui_yolo", test_tui_yolo, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tui_combines_runtime_options", test_tui_combines_runtime_options, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tui_session_option", test_tui_session_option, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_removed_set_session_id", test_rejects_removed_set_session_id,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tui_rejects_headless_options", test_tui_rejects_headless_options, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/run_options", test_run_options, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/session_id_requires_value", test_session_id_requires_value, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/effort_alias", test_effort_alias, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/reasoning_effort_rejects_unknown",
     test_reasoning_effort_rejects_unknown, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/profile_accepts_custom_name", test_profile_accepts_custom_name, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/benchmark_sets_presets", test_benchmark_sets_presets, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/explicit_headless_controls", test_explicit_headless_controls, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/prompt_conflict", test_prompt_conflict, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/acp_mode", test_acp_mode, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/acp_yolo", test_acp_yolo, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/acp_rejects_other_options", test_acp_rejects_other_options, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/self_test_mode", test_self_test_mode, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/max_turns_rejects_overflow", test_max_turns_rejects_overflow, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite cli_args_suite = {"/cli_args", tests, NULL, 1,
                             MUNIT_SUITE_OPTION_NONE};
