#include "finder.h"
#include "munit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static MunitResult test_ignore_directory_pattern(const MunitParameter params[],
                                                 void *data) {
  (void)params;
  (void)data;
  FinderIgnoreList ignore;
  finder_ignore_init(&ignore);
  munit_assert_int(finder_ignore_add(&ignore, "build/"), ==, 1);
  munit_assert_int(finder_path_ignored(&ignore, "build", 1), ==, 1);
  munit_assert_int(finder_path_ignored(&ignore, "build/app.o", 0), ==, 0);
  finder_ignore_free(&ignore);
  return MUNIT_OK;
}

static MunitResult test_ignore_file_pattern(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;
  FinderIgnoreList ignore;
  finder_ignore_init(&ignore);
  munit_assert_int(finder_ignore_add(&ignore, "*.o"), ==, 1);
  munit_assert_int(finder_path_ignored(&ignore, "src/main.o", 0), ==, 1);
  munit_assert_int(finder_path_ignored(&ignore, "src/main.c", 0), ==, 0);
  finder_ignore_free(&ignore);
  return MUNIT_OK;
}

static MunitResult test_ignore_negation(const MunitParameter params[],
                                        void *data) {
  (void)params;
  (void)data;
  FinderIgnoreList ignore;
  finder_ignore_init(&ignore);
  munit_assert_int(finder_ignore_add(&ignore, "*.log"), ==, 1);
  munit_assert_int(finder_ignore_add(&ignore, "!keep.log"), ==, 1);
  munit_assert_int(finder_path_ignored(&ignore, "debug.log", 0), ==, 1);
  munit_assert_int(finder_path_ignored(&ignore, "logs/keep.log", 0), ==, 0);
  finder_ignore_free(&ignore);
  return MUNIT_OK;
}

static MunitResult test_collect_files_honors_gitignore(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char dir[4096];
  snprintf(dir, sizeof(dir), "/tmp/capstan-finder-%ld", (long)getpid());
  rmdir(dir);
  munit_assert_int(mkdir(dir, 0700), ==, 0);

  char ignored_dir[4096];
  snprintf(ignored_dir, sizeof(ignored_dir), "%s/build", dir);
  munit_assert_int(mkdir(ignored_dir, 0700), ==, 0);

  char keep[4096], ignored[4096], gitignore[4096];
  snprintf(keep, sizeof(keep), "%s/keep.txt", dir);
  snprintf(ignored, sizeof(ignored), "%s/build/out.txt", dir);
  snprintf(gitignore, sizeof(gitignore), "%s/.gitignore", dir);

  FILE *f = fopen(keep, "w");
  munit_assert_not_null(f);
  fputs("keep", f);
  fclose(f);
  f = fopen(ignored, "w");
  munit_assert_not_null(f);
  fputs("ignored", f);
  fclose(f);
  f = fopen(gitignore, "w");
  munit_assert_not_null(f);
  fputs("build/\n", f);
  fclose(f);

  FinderIgnoreList ignore;
  finder_ignore_init(&ignore);
  munit_assert_int(finder_ignore_load_file(&ignore, dir, ".gitignore"), ==, 1);
  PopupItem *items = NULL;
  int count = 0;
  munit_assert_int(finder_collect_files(dir, &ignore, &items, &count), ==, 1);

  int saw_keep = 0;
  int saw_ignored = 0;
  for (int i = 0; i < count; i++) {
    if (strcmp(items[i].text, "keep.txt") == 0)
      saw_keep = 1;
    if (strcmp(items[i].text, "build/out.txt") == 0)
      saw_ignored = 1;
    free(items[i].text);
    free(items[i].value);
  }
  free(items);
  finder_ignore_free(&ignore);

  munit_assert_int(saw_keep, ==, 1);
  munit_assert_int(saw_ignored, ==, 0);

  unlink(keep);
  unlink(ignored);
  unlink(gitignore);
  rmdir(ignored_dir);
  rmdir(dir);
  return MUNIT_OK;
}

static MunitResult test_fuzzy_scores_match_basename(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int base_score = finder_fuzzy_score("src/deep_needle.txt", "needle");
  int path_score = finder_fuzzy_score("needle_dir/deep.txt", "needle");
  munit_assert_int(base_score, >, 0);
  munit_assert_int(path_score, >, 0);
  munit_assert_int(base_score, >, path_score);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/ignore_directory_pattern", test_ignore_directory_pattern, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/ignore_file_pattern", test_ignore_file_pattern, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/ignore_negation", test_ignore_negation, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/collect_files_honors_gitignore", test_collect_files_honors_gitignore,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/fuzzy_scores_match_basename", test_fuzzy_scores_match_basename, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite finder_suite = {"/finder", tests, NULL, 1,
                           MUNIT_SUITE_OPTION_NONE};
