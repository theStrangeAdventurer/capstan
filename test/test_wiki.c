#include "munit.h"
#include "wiki.h"
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

static MunitResult test_builds_prompt_with_core_and_metadata_only(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-%ld", (long)getpid());
  rmdir(root);
  make_dir(root);

  char profile[256];
  snprintf(profile, sizeof(profile), "%s/profile", root);
  make_dir(profile);
  char contexts[256];
  snprintf(contexts, sizeof(contexts), "%s/contexts", root);
  make_dir(contexts);

  char core[256];
  snprintf(core, sizeof(core), "%s/core.md", profile);
  write_file(core, "Owner prefers concise answers.\n");

  char homelab[256];
  snprintf(homelab, sizeof(homelab), "%s/homelab.md", contexts);
  write_file(homelab,
             "---\n"
             "schema_version: 1\n"
             "id: home-devices\n"
             "kind: resource\n"
             "title: Home devices\n"
             "description: Домашние устройства владельца.\n"
             "use_when:\n"
             "  - Пользователь спрашивает про NAS.\n"
             "tags:\n"
             "  - homelab\n"
             "  - ssh\n"
             "index_policy: always\n"
             "context_policy: retrieve_only\n"
             "---\n"
             "SECRET BODY MUST NOT BE INDEXED\n");

  char *prompt = wiki_build_prompt(root);
  munit_assert_not_null(prompt);
  munit_assert_true(strstr(prompt, "# Wiki") != NULL);
  munit_assert_true(strstr(prompt, "profile/core.md") != NULL);
  munit_assert_true(strstr(prompt, "Owner prefers concise answers.") != NULL);
  munit_assert_true(strstr(prompt, "Path: contexts/homelab.md") != NULL);
  munit_assert_true(strstr(prompt, "ID: home-devices") != NULL);
  munit_assert_true(strstr(prompt, "Title: Home devices") != NULL);
  munit_assert_true(strstr(prompt, "Use when: Пользователь спрашивает про NAS.") != NULL);
  munit_assert_true(strstr(prompt, "Tags: homelab; ssh") != NULL);
  munit_assert_true(strstr(prompt, "SECRET BODY MUST NOT BE INDEXED") == NULL);
  free(prompt);

  char *summary = wiki_build_summary(root);
  munit_assert_not_null(summary);
  munit_assert_true(strstr(summary, "Profile core: loaded") != NULL);
  munit_assert_true(strstr(summary, "Indexed documents: 1") != NULL);
  munit_assert_true(strstr(summary, "- contexts/homelab.md") != NULL);
  free(summary);

  unlink(homelab);
  unlink(core);
  rmdir(contexts);
  rmdir(profile);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_missing_config_is_empty_prompt_and_status_summary(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char *prompt = wiki_build_prompt(NULL);
  munit_assert_not_null(prompt);
  munit_assert_string_equal(prompt, "");
  free(prompt);

  char *summary = wiki_build_summary(NULL);
  munit_assert_not_null(summary);
  munit_assert_true(strstr(summary, "Wiki is not configured") != NULL);
  free(summary);
  return MUNIT_OK;
}

static MunitResult test_builtin_wiki_onboarding_skill_is_indexed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *content =
      "---\n"
      "name: wiki-onboarding\n"
      "description: Use when wiki is not configured.\n"
      "---\n"
      "Guide wiki setup.\n";
  BuiltinSkill builtin[] = {{"wiki-onboarding",
                             "embedded:skills/wiki-onboarding/SKILL.md",
                             content, strlen(content)}};
  char *prompt = skills_build_prompt(builtin, 1, NULL, NULL, NULL);
  munit_assert_not_null(prompt);
  munit_assert_true(strstr(prompt, "Skill: wiki-onboarding") != NULL);
  munit_assert_true(strstr(prompt, "embedded:skills/wiki-onboarding/SKILL.md") != NULL);
  free(prompt);
  return MUNIT_OK;
}

static MunitResult test_builds_prompt_with_ingested_source_index(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[256];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-index-%ld", (long)getpid());
  rmdir(root);
  make_dir(root);

  char index_dir[256];
  snprintf(index_dir, sizeof(index_dir), "%s/index", root);
  make_dir(index_dir);
  char index_file[256];
  snprintf(index_file, sizeof(index_file), "%s/docs-12345678.json", index_dir);
  write_file(index_file,
             "{"
             "\"schema_version\":1,"
             "\"source_id\":\"docs-12345678\","
             "\"source_root\":\"/tmp/source-docs\","
             "\"entries\":[{"
             "\"path\":\"guide.md\","
             "\"id\":\"guide\","
             "\"kind\":\"source\","
             "\"title\":\"Guide\","
             "\"description\":\"External guide metadata.\","
             "\"use_when\":[\"The user asks about the guide.\"],"
             "\"tags\":[\"docs\",\"guide\"],"
             "\"index_policy\":\"always\","
             "\"context_policy\":\"retrieve_only\""
             "}]"
             "}");

  char *prompt = wiki_build_prompt(root);
  munit_assert_not_null(prompt);
  munit_assert_true(strstr(prompt, "Path: source:docs-12345678:guide.md") != NULL);
  munit_assert_true(strstr(prompt, "Source: docs-12345678") != NULL);
  munit_assert_true(strstr(prompt, "Source path: guide.md") != NULL);
  munit_assert_true(strstr(prompt, "wiki_source_read {\"source\":\"docs-12345678\",\"path\":\"guide.md\"}") != NULL);
  munit_assert_true(strstr(prompt, "External guide metadata.") != NULL);
  free(prompt);

  char *summary = wiki_build_summary(root);
  munit_assert_not_null(summary);
  munit_assert_true(strstr(summary, "Indexed documents: 1") != NULL);
  munit_assert_true(strstr(summary, "source:docs-12345678:guide.md") != NULL);
  free(summary);

  unlink(index_file);
  rmdir(index_dir);
  rmdir(root);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/builds_prompt_with_core_and_metadata_only",
     test_builds_prompt_with_core_and_metadata_only, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/missing_config_is_empty_prompt_and_status_summary",
     test_missing_config_is_empty_prompt_and_status_summary, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/builtin_wiki_onboarding_skill_is_indexed",
     test_builtin_wiki_onboarding_skill_is_indexed, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/builds_prompt_with_ingested_source_index",
     test_builds_prompt_with_ingested_source_index, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite wiki_suite = {"/wiki", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
