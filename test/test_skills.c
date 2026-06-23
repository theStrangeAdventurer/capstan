#include "munit.h"
#include "skills.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  munit_assert_not_null(f);
  fputs(content, f);
  fclose(f);
}

static void make_dir(const char *path) {
  munit_assert_int(mkdir(path, 0700), ==, 0);
}

static MunitResult test_loads_skill_md_and_resource_manifest(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-skills-%ld", (long)getpid());
  rmdir(root);
  make_dir(root);

  char skills_dir[256];
  snprintf(skills_dir, sizeof(skills_dir), "%s/skills", root);
  make_dir(skills_dir);

  char nested[256];
  snprintf(nested, sizeof(nested), "%s/code-review", skills_dir);
  make_dir(nested);

  char nested_skill[256];
  snprintf(nested_skill, sizeof(nested_skill), "%s/SKILL.md", nested);
  write_file(nested_skill,
             "---\n"
             "name: review-helper\n"
             "description: Use for focused code review.\n"
             "---\n"
             "\n"
             "Review instructions");

  char references[256];
  snprintf(references, sizeof(references), "%s/references", nested);
  make_dir(references);

  char scripts[256];
  snprintf(scripts, sizeof(scripts), "%s/scripts", nested);
  make_dir(scripts);

  char reference_file[256];
  snprintf(reference_file, sizeof(reference_file), "%s/checklist.md",
           references);
  write_file(reference_file, "Reference details");

  char script_file[256];
  snprintf(script_file, sizeof(script_file), "%s/run.sh", scripts);
  write_file(script_file, "echo ok");

  char file_skill[256];
  snprintf(file_skill, sizeof(file_skill), "%s/debug.md", skills_dir);
  write_file(file_skill, "Debug instructions");

  char *prompt = skills_build_prompt(NULL, skills_dir, NULL, NULL);
  munit_assert_not_null(prompt);
  munit_assert_true(strstr(prompt, "## Skill: code-review") != NULL);
  munit_assert_true(strstr(prompt, "Name: review-helper") != NULL);
  munit_assert_true(strstr(prompt, "Description: Use for focused code review.") !=
                    NULL);
  munit_assert_true(strstr(prompt, "SKILL.md") != NULL);
  munit_assert_true(strstr(prompt, "Mandatory skill use rule") != NULL);
  munit_assert_true(strstr(prompt, "must read that skill's `Skill file` path "
                                  "completely") != NULL);
  munit_assert_true(strstr(prompt, "Do not apply a skill from this index alone") !=
                    NULL);
  munit_assert_true(strstr(prompt, "Review instructions") == NULL);
  munit_assert_true(strstr(prompt, "Resources:") == NULL);
  munit_assert_true(strstr(prompt, "- references/checklist.md") == NULL);
  munit_assert_true(strstr(prompt, "- scripts/run.sh") == NULL);
  munit_assert_true(strstr(prompt, "## Skill: debug") == NULL);
  munit_assert_true(strstr(prompt, "Debug instructions") == NULL);
  free(prompt);

  char *summary = skills_build_summary(NULL, skills_dir, NULL, NULL);
  munit_assert_not_null(summary);
  munit_assert_true(strstr(summary, "Loaded skills: 1") != NULL);
  munit_assert_true(strstr(summary, "- code-review [project]") != NULL);
  munit_assert_true(strstr(summary, "Name: review-helper") != NULL);
  munit_assert_true(strstr(summary, "Description: Use for focused code review.") !=
                    NULL);
  munit_assert_true(strstr(summary, "SKILL.md:") != NULL);
  munit_assert_true(strstr(summary, "Resource root:") != NULL);
  munit_assert_true(strstr(summary, "    - references/checklist.md") != NULL);
  munit_assert_true(strstr(summary, "    - scripts/run.sh") != NULL);
  free(summary);

  unlink(file_skill);
  unlink(script_file);
  unlink(reference_file);
  unlink(nested_skill);
  rmdir(scripts);
  rmdir(references);
  rmdir(nested);
  rmdir(skills_dir);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_project_skill_overrides_user_skill(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-skills-override-%ld",
           (long)getpid());
  rmdir(root);
  make_dir(root);

  char user_dir[256];
  char project_dir[256];
  snprintf(user_dir, sizeof(user_dir), "%s/user", root);
  snprintf(project_dir, sizeof(project_dir), "%s/project", root);
  make_dir(user_dir);
  make_dir(project_dir);

  char user_skill[256];
  char project_skill[256];
  char user_skill_dir[256];
  char project_skill_dir[256];
  snprintf(user_skill_dir, sizeof(user_skill_dir), "%s/test", user_dir);
  snprintf(project_skill_dir, sizeof(project_skill_dir), "%s/test",
           project_dir);
  make_dir(user_skill_dir);
  make_dir(project_skill_dir);
  snprintf(user_skill, sizeof(user_skill), "%s/SKILL.md", user_skill_dir);
  snprintf(project_skill, sizeof(project_skill), "%s/SKILL.md",
           project_skill_dir);
  write_file(user_skill,
             "---\nname: user-test\ndescription: user copy\n---\nbody");
  write_file(project_skill,
             "---\nname: project-test\ndescription: project copy\n---\nbody");

  char *prompt = skills_build_prompt(NULL, project_dir, user_dir, NULL);
  munit_assert_not_null(prompt);
  munit_assert_true(strstr(prompt, "Name: project-test") != NULL);
  munit_assert_true(strstr(prompt, "project copy") != NULL);
  munit_assert_true(strstr(prompt, "user copy") == NULL);
  munit_assert_true(strstr(prompt, "Source: project") != NULL);
  munit_assert_true(strstr(prompt, "body") == NULL);
  free(prompt);

  unlink(user_skill);
  unlink(project_skill);
  rmdir(user_skill_dir);
  rmdir(project_skill_dir);
  rmdir(user_dir);
  rmdir(project_dir);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_common_skill_loaded_and_overridden(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-skills-common-%ld",
           (long)getpid());
  rmdir(root);
  make_dir(root);

  char common_dir[256];
  char user_dir[256];
  char project_dir[256];
  snprintf(common_dir, sizeof(common_dir), "%s/common", root);
  snprintf(user_dir, sizeof(user_dir), "%s/user", root);
  snprintf(project_dir, sizeof(project_dir), "%s/project", root);
  make_dir(common_dir);
  make_dir(user_dir);
  make_dir(project_dir);

  char common_shared_dir[256];
  char user_shared_dir[256];
  char common_only_dir[256];
  snprintf(common_shared_dir, sizeof(common_shared_dir), "%s/shared",
           common_dir);
  snprintf(user_shared_dir, sizeof(user_shared_dir), "%s/shared", user_dir);
  snprintf(common_only_dir, sizeof(common_only_dir), "%s/common-only",
           common_dir);
  make_dir(common_shared_dir);
  make_dir(user_shared_dir);
  make_dir(common_only_dir);

  char common_shared_skill[256];
  char user_shared_skill[256];
  char common_only_skill[256];
  snprintf(common_shared_skill, sizeof(common_shared_skill), "%s/SKILL.md",
           common_shared_dir);
  snprintf(user_shared_skill, sizeof(user_shared_skill), "%s/SKILL.md",
           user_shared_dir);
  snprintf(common_only_skill, sizeof(common_only_skill), "%s/SKILL.md",
           common_only_dir);
  write_file(common_shared_skill,
             "---\nname: common-shared\ndescription: common copy\n---\nbody");
  write_file(user_shared_skill,
             "---\nname: user-shared\ndescription: user copy\n---\nbody");
  write_file(common_only_skill,
             "---\nname: common-only\ndescription: common only\n---\nbody");

  char *summary =
      skills_build_summary(NULL, project_dir, user_dir, common_dir);
  munit_assert_not_null(summary);
  munit_assert_true(strstr(summary, "Loaded skills: 2") != NULL);
  munit_assert_true(strstr(summary, "- shared [user]") != NULL);
  munit_assert_true(strstr(summary, "Name: user-shared") != NULL);
  munit_assert_true(strstr(summary, "common copy") == NULL);
  munit_assert_true(strstr(summary, "- common-only [common]") != NULL);
  free(summary);

  unlink(common_shared_skill);
  unlink(user_shared_skill);
  unlink(common_only_skill);
  rmdir(common_shared_dir);
  rmdir(user_shared_dir);
  rmdir(common_only_dir);
  rmdir(common_dir);
  rmdir(user_dir);
  rmdir(project_dir);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_empty_when_no_skills(const MunitParameter params[],
                                             void *data) {
  (void)params;
  (void)data;
  char *prompt = skills_build_prompt(NULL, "/tmp/capstan-missing-project-skills",
                                     "/tmp/capstan-missing-user-skills",
                                     "/tmp/capstan-missing-common-skills");
  munit_assert_not_null(prompt);
  munit_assert_string_equal(prompt, "");
  free(prompt);
  return MUNIT_OK;
}

static MunitResult test_ignores_directory_without_skill_md(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-skills-readme-%ld",
           (long)getpid());
  rmdir(root);
  make_dir(root);

  char skills_dir[256];
  snprintf(skills_dir, sizeof(skills_dir), "%s/skills", root);
  make_dir(skills_dir);

  char readme_skill_dir[256];
  snprintf(readme_skill_dir, sizeof(readme_skill_dir), "%s/readme-only",
           skills_dir);
  make_dir(readme_skill_dir);

  char readme[256];
  snprintf(readme, sizeof(readme), "%s/README.md", readme_skill_dir);
  write_file(readme, "README should be ignored");

  char *prompt = skills_build_prompt(NULL, skills_dir, NULL, NULL);
  munit_assert_not_null(prompt);
  munit_assert_string_equal(prompt, "");
  free(prompt);

  unlink(readme);
  rmdir(readme_skill_dir);
  rmdir(skills_dir);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_builtin_skill_loaded_and_overridden(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-skills-builtin-%ld",
           (long)getpid());
  rmdir(root);
  make_dir(root);

  char builtin_dir[256];
  char user_dir[256];
  snprintf(builtin_dir, sizeof(builtin_dir), "%s/builtin", root);
  snprintf(user_dir, sizeof(user_dir), "%s/user", root);
  make_dir(builtin_dir);
  make_dir(user_dir);

  char builtin_skill_dir[256];
  char user_skill_dir[256];
  snprintf(builtin_skill_dir, sizeof(builtin_skill_dir), "%s/self-improvement",
           builtin_dir);
  snprintf(user_skill_dir, sizeof(user_skill_dir), "%s/self-improvement",
           user_dir);
  make_dir(builtin_skill_dir);
  make_dir(user_skill_dir);

  char builtin_skill[256];
  char user_skill[256];
  snprintf(builtin_skill, sizeof(builtin_skill), "%s/SKILL.md",
           builtin_skill_dir);
  snprintf(user_skill, sizeof(user_skill), "%s/SKILL.md", user_skill_dir);
  write_file(builtin_skill,
             "---\nname: builtin-self\ndescription: built in\n---\nbody");
  write_file(user_skill,
             "---\nname: user-self\ndescription: user override\n---\nbody");

  char *summary = skills_build_summary(builtin_dir, NULL, user_dir, NULL);
  munit_assert_not_null(summary);
  munit_assert_true(strstr(summary, "Loaded skills: 1") != NULL);
  munit_assert_true(strstr(summary, "- self-improvement [user]") != NULL);
  munit_assert_true(strstr(summary, "Name: user-self") != NULL);
  munit_assert_true(strstr(summary, "built in") == NULL);
  free(summary);

  unlink(builtin_skill);
  unlink(user_skill);
  rmdir(builtin_skill_dir);
  rmdir(user_skill_dir);
  rmdir(builtin_dir);
  rmdir(user_dir);
  rmdir(root);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/loads_skill_md_and_resource_manifest",
     test_loads_skill_md_and_resource_manifest, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/project_skill_overrides_user_skill",
     test_project_skill_overrides_user_skill, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/common_skill_loaded_and_overridden",
     test_common_skill_loaded_and_overridden, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/empty_when_no_skills", test_empty_when_no_skills, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/ignores_directory_without_skill_md",
     test_ignores_directory_without_skill_md, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/builtin_skill_loaded_and_overridden",
     test_builtin_skill_loaded_and_overridden, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite skills_suite = {"/skills", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
