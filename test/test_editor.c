#include "editor.h"
#include "munit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static MunitResult test_limits(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char path[] = "build/editor-prompt-test.XXXXXX";
  int fd = mkstemp(path);
  munit_assert_int(fd, >=, 0);
  FILE *file = fdopen(fd, "wb");
  munit_assert_not_null(file);
  char content[8193];
  memset(content, 'x', 8190);
  content[8190] = '\n';
  munit_assert_size(fwrite(content, 1, 8191, file), ==, 8191);
  munit_assert_int(fclose(file), ==, 0);
  char destination[8192], error[256];
  munit_assert_int(editor_read_prompt_file(path, destination, sizeof(destination), error, sizeof(error)), ==, 0);
  munit_assert_size(strlen(destination), ==, 8191);
  munit_assert_memory_equal(8191, destination, content);
  munit_assert_string_equal(error, "");

  /* A UTF-8 codepoint straddles the byte limit: never import its prefix. */
  content[8190] = (char)0xe2;
  content[8191] = (char)0x82;
  content[8192] = (char)0xac;
  file = fopen(path, "wb");
  munit_assert_not_null(file);
  munit_assert_size(fwrite(content, 1, sizeof(content), file), ==, sizeof(content));
  munit_assert_int(fclose(file), ==, 0);
  memset(destination, 'D', sizeof(destination));
  char original[sizeof(destination)];
  memcpy(original, destination, sizeof(original));
  munit_assert_int(editor_read_prompt_file(path, destination, sizeof(destination), error, sizeof(error)), ==, -1);
  munit_assert_string_equal(error, "Prompt exceeds 8191-byte input limit");
  munit_assert_memory_equal(sizeof(destination), destination, original);
  file = fopen(path, "rb");
  munit_assert_not_null(file);
  char retained[sizeof(content)];
  munit_assert_size(fread(retained, 1, sizeof(retained), file), ==, sizeof(content));
  munit_assert_memory_equal(sizeof(content), retained, content);
  fclose(file);
  unlink(path);
  return MUNIT_OK;
}

static MunitResult test_failures(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char path[] = "build/editor-prompt-test.XXXXXX";
  int fd = mkstemp(path);
  munit_assert_int(fd, >=, 0);
  munit_assert_int(close(fd), ==, 0);
  char destination[32] = "original draft", error[256];
  /* Empty files are valid drafts. */
  munit_assert_int(editor_read_prompt_file(path, destination, sizeof(destination), error, sizeof(error)), ==, 0);
  munit_assert_string_equal(destination, "");
  strcpy(destination, "original draft");
  FILE *file = fopen(path, "wb");
  munit_assert_not_null(file);
  munit_assert_size(fwrite("a\0b", 1, 3, file), ==, 3);
  fclose(file);
  munit_assert_int(editor_read_prompt_file(path, destination, sizeof(destination), error, sizeof(error)), ==, -1);
  munit_assert_string_equal(error, "Prompt contains a NUL byte");
  munit_assert_string_equal(destination, "original draft");
  unlink(path);
  munit_assert_int(editor_read_prompt_file(path, destination, sizeof(destination), error, sizeof(error)), ==, -1);
  munit_assert_not_null(strstr(error, "Cannot open prompt:"));
  munit_assert_string_equal(destination, "original draft");
  /* Directories open on supported platforms, but cannot be read as files. */
  munit_assert_int(mkdir(path, 0700), ==, 0);
  munit_assert_int(editor_read_prompt_file(path, destination, sizeof(destination), error, sizeof(error)), ==, -1);
  munit_assert_not_null(strstr(error, "Cannot read prompt:"));
  munit_assert_string_equal(destination, "original draft");
  munit_assert_int(rmdir(path), ==, 0);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/limits", test_limits, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/failures", test_failures, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite editor_suite = {"/editor", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};

