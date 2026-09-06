#define _DARWIN_C_SOURCE
#include "munit.h"
#include "workspace_status.h"
#include "shell_process.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static MunitResult test_parse(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  WorkspaceStatus status;
  munit_assert_true(workspace_status_parse("repo\nstats\ndone\n", &status));
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_READY);
  munit_assert_ullong(status.files, ==, 0);
  munit_assert_true(workspace_status_parse(
      "repo\nMM source.c\nR  old -> new\n M image.png\n?? dir/new\n"
      "?? \"with\\nnewline\"\nUU conflict\nstats\n"
      "12\t4\tsource.c\n0\t0\told => new\n-\t-\timage.png\n"
      "3\t2\t\"with\\nnewline\"\ndone\n", &status));
  munit_assert_ullong(status.files, ==, 6);
  munit_assert_ullong(status.added, ==, 15);
  munit_assert_ullong(status.deleted, ==, 6);
  munit_assert_true(workspace_status_parse("none\n", &status));
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_NONE);
  const char *bad[] = {"", "repo\n", "repo\n M x\n", "repo\nstats\n1\t2\tx\n",
      "repo\nstats\n18446744073709551616\t0\tx\ndone\n",
      "repo\nstats\n1\tno\tx\ndone\n", "none\nextra", NULL};
  for (int i = 0; bad[i]; i++) {
    munit_assert_false(workspace_status_parse(bad[i], &status));
    munit_assert_int(status.state, ==, WORKSPACE_STATUS_ERROR);
    munit_assert_ullong(status.files, ==, 0);
  }
  return MUNIT_OK;
}

static void run(const char *root, const char *command) {
  ShellProcessResult result;
  munit_assert_true(shell_process_run(command, root, 10, 4096, 4096, NULL, &result));
  if (result.exit_code != 0) munit_errorf("fixture failed: %s", result.stderr_text);
  shell_process_result_free(&result);
}

static WorkspaceStatus collect(const char *root) {
  workspace_status_shutdown();
  char *script = realpath("agent/vcs_git_stats.sh", NULL);
  munit_assert_not_null(script);
  const char *argv[] = {"sh", script, NULL};
  workspace_status_configure("git", argv, 1);
  free(script);
  WorkspaceStatus value = *workspace_status_poll(root);
  for (int i = 0; i < 650 && value.state == WORKSPACE_STATUS_PENDING; i++) {
    usleep(10000);
    value = *workspace_status_poll(root);
  }
  munit_assert_int(value.state, !=, WORKSPACE_STATUS_PENDING);
  return value;
}

static MunitResult test_git(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char template[] = "build/workspace-status-XXXXXX";
  munit_assert_not_null(mkdtemp(template));
  char *root = realpath(template, NULL);
  munit_assert_not_null(root);
  const char *old_ceiling = getenv("GIT_CEILING_DIRECTORIES");
  char *saved_ceiling = old_ceiling ? my_strdup(old_ceiling) : NULL;
  /* Do not discover Capstan's parent repository in this isolated fixture. */
  setenv("GIT_CEILING_DIRECTORIES", root, 1);
  run(root, "mkdir empty");
  char empty[4096];
  snprintf(empty, sizeof(empty), "%s/empty", root);
  WorkspaceStatus status = collect(empty);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_NONE);
  run(root, "git init -q; git config user.name Fixture; git config user.email fixture@example.invalid; "
            "git config core.hooksPath /dev/null; "
            "printf 'first\\nsecond\\n' > tracked; git add tracked; "
            "printf 'first\\nsecond\\nthird\\n' > tracked");
  unsetenv("GIT_CEILING_DIRECTORIES"); /* The fixture now owns its repository. */
  status = collect(root);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_READY);
  munit_assert_ullong(status.files, ==, 1);
  munit_assert_ullong(status.added, ==, 3); /* unborn: net, not staged + unstaged */
  munit_assert_ullong(status.deleted, ==, 0);
  run(root, "printf 'old\\n' > tracked; printf 'drop\\n' > deleted; "
            "printf 'rename\\n' > rename; printf '\\000old' > image; "
            "printf 'ignored\\n' > .gitignore; "
            "git add .; git -c commit.gpgsign=false commit -qm initial");
  status = collect(root);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_READY);
  munit_assert_ullong(status.files, ==, 0);
  run(root, "printf 'staged\\nextra\\n' > tracked; git add tracked; "
            "printf 'final\\n' > tracked; rm deleted; "
            "git mv rename 'new name'; printf '\\000new' > image; "
            "touch 'with\nnewline'; mkdir scope; touch scope/untracked ignored");
  status = collect(root);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_READY);
  munit_assert_ullong(status.files, ==, 6);
  munit_assert_ullong(status.added, ==, 1);
  munit_assert_ullong(status.deleted, ==, 2);
  char scope[4096];
  snprintf(scope, sizeof(scope), "%s/scope", root);
  /* A workspace switch discards the old repository's cached counts. */
  munit_assert_int(workspace_status_poll(scope)->state, ==, WORKSPACE_STATUS_PENDING);
  status = collect(scope);
  munit_assert_ullong(status.files, ==, 1);
  munit_assert_ullong(status.added, ==, 0);
  munit_assert_ullong(status.deleted, ==, 0);
  run(root, "touch scope/another");
  for (int i = 0; i < 500 && status.files != 2; i++) {
    usleep(10000);
    status = *workspace_status_poll(scope);
  }
  munit_assert_ullong(status.files, ==, 2); /* external changes refresh automatically */
  snprintf(scope, sizeof(scope), "%s/missing", root);
  status = collect(scope);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_ERROR);
  workspace_status_shutdown();
  run(root, "rm -rf .git empty scope tracked deleted image 'new name' 'with\nnewline' .gitignore ignored");
  munit_assert_int(rmdir(root), ==, 0);
  if (saved_ceiling) setenv("GIT_CEILING_DIRECTORIES", saved_ceiling, 1);
  else unsetenv("GIT_CEILING_DIRECTORIES");
  free(saved_ceiling);
  free(root);
  return MUNIT_OK;
}

static WorkspaceStatus wait_custom(const char *root) {
  WorkspaceStatus status = *workspace_status_poll(root);
  for (int i = 0; i < 650 && status.state == WORKSPACE_STATUS_PENDING; i++) {
    usleep(10000);
    status = *workspace_status_poll(root);
  }
  return status;
}

static MunitResult test_custom(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  WorkspaceStatus status;
  munit_assert_true(workspace_status_parse_files(
      "files-v1\nM\t12\t3\tfile with spaces\nR\t0\t0\tnew name\n"
      "?\t-\t-\tuntracked\\nname\nM\t-\t-\tbinary\ndone\n", &status));
  munit_assert_ullong(status.files, ==, 4);
  munit_assert_ullong(status.added, ==, 12);
  munit_assert_ullong(status.deleted, ==, 3);
  const char *bad[] = {"", "files-v1\n", "files-v1\nM\t1\t2\tf\n",
      "files-v1\nM\t1\t2\t\ndone\n", "files-v1\nM\t1\t2\tx\ty\ndone\n",
      "files-v1\nX\t1\t2\tf\ndone\n", "files-v1\ndone\nextra",
      "files-v1\nM\t18446744073709551615\t0\tf\nM\t1\t0\tg\ndone\n", NULL};
  for (int i = 0; bad[i]; i++) {
    munit_assert_false(workspace_status_parse_files(bad[i], &status));
    munit_assert_int(status.state, ==, WORKSPACE_STATUS_ERROR);
    munit_assert_ullong(status.files, ==, 0);
  }
  char template[] = "build/workspace-custom-XXXXXX";
  munit_assert_not_null(mkdtemp(template));
  const char *argv[] = {"printf", "%s", "files-v1\nM\t7\t2\t$(touch injected)\ndone\n", NULL};
  workspace_status_configure("custom", argv, 0);
  status = wait_custom(template);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_READY);
  munit_assert_ullong(status.files, ==, 1);
  munit_assert_ullong(status.added, ==, 7);
  munit_assert_ullong(status.deleted, ==, 2);
  /* An identical descriptor retains the cached result. */
  workspace_status_configure("custom", argv, 0);
  munit_assert_int(workspace_status_poll(template)->state, ==, WORKSPACE_STATUS_READY);
  const char *clean[] = {"printf", "files-v1\ndone\n", NULL};
  workspace_status_configure("custom", clean, 0);
  munit_assert_int(workspace_status_poll(template)->state, ==, WORKSPACE_STATUS_PENDING);
  status = wait_custom(template);
  munit_assert_ullong(status.files, ==, 0);
  const char *slow[] = {"sh", "-c", "sleep 10", NULL};
  workspace_status_configure("slow", slow, 0);
  workspace_status_poll(template);
  workspace_status_configure("other", argv, 0); /* cancel an in-flight worker */
  status = wait_custom(template);
  munit_assert_int(status.state, ==, WORKSPACE_STATUS_READY);
  munit_assert_ullong(status.added, ==, 7);
  workspace_status_configure("unsupported", NULL, 0);
  munit_assert_int(workspace_status_poll(template)->state, ==, WORKSPACE_STATUS_ERROR);
  const char *failed[] = {"sh", "-c", "printf 'files-v1\\ndone\\n'; exit 1", NULL};
  workspace_status_configure("failed", failed, 0);
  munit_assert_int(wait_custom(template).state, ==, WORKSPACE_STATUS_ERROR);
  workspace_status_configure("timeout", slow, 0);
  munit_assert_int(wait_custom(template).state, ==, WORKSPACE_STATUS_ERROR);
  workspace_status_shutdown();
  munit_assert_int(rmdir(template), ==, 0); /* argv text was not shell-expanded */
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/custom", test_custom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/parse", test_parse, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/git", test_git, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};
MunitSuite workspace_status_suite = {"/workspace_status", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
