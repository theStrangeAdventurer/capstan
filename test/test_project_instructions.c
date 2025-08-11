#include "munit.h"
#include "project_instructions.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_dir(const char *path) {
  munit_assert_int(mkdir(path, 0700), ==, 0);
}

static void make_temp_dir(char *path, size_t path_size) {
  for (int i = 0; i < 1000; i++) {
    snprintf(path, path_size, "/tmp/capstan-instructions-%ld-%d",
             (long)getpid(), i);
    if (mkdir(path, 0700) == 0)
      return;
    if (errno != EEXIST)
      break;
  }
  munit_error("failed to create temporary instruction directory");
}

static void write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  munit_assert_not_null(file);
  munit_assert_size(fwrite(text, 1, strlen(text), file), ==, strlen(text));
  fclose(file);
}

static MunitResult test_config_user_and_project_instructions(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char root[512];
  make_temp_dir(root, sizeof(root));

  char config[512], workspace[512], config_agents[512], project_agents[512];
  snprintf(config, sizeof(config), "%s/config", root);
  snprintf(workspace, sizeof(workspace), "%s/workspace", root);
  make_dir(config);
  make_dir(workspace);
  snprintf(config_agents, sizeof(config_agents), "%s/AGENTS.md", config);
  snprintf(project_agents, sizeof(project_agents), "%s/AGENTS.md", workspace);
  write_text(config_agents, "user rules");
  write_text(project_agents, "project rules");

  char *prompt = project_instructions_build_prompt(workspace, config, root);
  munit_assert_not_null(prompt);
  const char *user = strstr(prompt, "# User Instructions");
  const char *project = strstr(prompt, "# Project Instructions");
  munit_assert_not_null(user);
  munit_assert_not_null(project);
  munit_assert_true(user < project);
  munit_assert_not_null(strstr(prompt, config_agents));
  munit_assert_not_null(strstr(prompt, "user rules"));
  munit_assert_not_null(strstr(prompt, project_agents));
  munit_assert_not_null(strstr(prompt, "project rules"));
  free(prompt);

  unlink(project_agents);
  unlink(config_agents);
  rmdir(workspace);
  rmdir(config);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_shared_user_fallback(const MunitParameter params[],
                                              void *data) {
  (void)params;
  (void)data;
  char root[512];
  make_temp_dir(root, sizeof(root));

  char config[512], shared[512], shared_agents[512];
  snprintf(config, sizeof(config), "%s/config", root);
  snprintf(shared, sizeof(shared), "%s/.agents", root);
  make_dir(config);
  make_dir(shared);
  snprintf(shared_agents, sizeof(shared_agents), "%s/AGENTS.md", shared);
  write_text(shared_agents, "shared rules");

  char *prompt = project_instructions_build_prompt(NULL, config, root);
  munit_assert_not_null(prompt);
  munit_assert_not_null(strstr(prompt, shared_agents));
  munit_assert_not_null(strstr(prompt, "shared rules"));
  free(prompt);

  unlink(shared_agents);
  rmdir(shared);
  rmdir(config);
  rmdir(root);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/config_user_and_project", test_config_user_and_project_instructions,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/shared_user_fallback", test_shared_user_fallback, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite project_instructions_suite = {
    "/project_instructions", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
