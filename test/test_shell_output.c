#include "munit.h"
#include "shell_output.h"
#include "linemap.h"
#include <stdio.h>
#include <string.h>

static void result(char *text, size_t lines) {
  strcpy(text, "command — done\n\n[exit 1] TIMED OUT\n");
  for (size_t i = 0; i < lines; i++) {
    char row[64];
    snprintf(row, sizeof(row), "строка %zu\t界\n", i + 1);
    strcat(text, row);
  }
}

static MunitResult test_threshold(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  for (size_t lines = 0; lines <= 21; lines++) {
    char text[4096], original[4096];
    result(text, lines);
    strcpy(original, text);
    ShellOutput output = {0};
    size_t start = (size_t)(strstr(text, "— done") - text);
    munit_assert_true(shell_output_add(&output, text, start, strlen(text)));
    const char *view = shell_output_build(&output, text);
    munit_assert_size(output.blocks[0].lines, ==, lines);
    const char *header = "command — done [exit 1] TIMED OUT\n";
    munit_assert_int(strncmp(view, header, strlen(header)), ==, 0);
    size_t status = (size_t)(strstr(text, "[exit 1]") - text);
    munit_assert_size(shell_output_offset(&output, status), ==,
                      (size_t)(strstr(view, "[exit 1]") - view));
    munit_assert_size(shell_output_offset(&output, status - 1), ==,
                      strlen("command — done"));
    if (lines <= 20) {
      munit_assert_string_equal(view + strlen(header),
                                text + output.blocks[0].body_start);
      munit_assert_int(shell_output_control(&output, start), ==, -1);
    } else {
      munit_assert_string_equal(view + strlen(header), "[+] 21 lines\n");
      munit_assert_null(strstr(view, "строка"));
      size_t control = output.blocks[0].control_start;
      for (size_t p = control; p < control + 3; p++)
        munit_assert_int(shell_output_control(&output, p), ==, 0);
      munit_assert_int(shell_output_control(&output, control + 3), ==, -1);
      munit_assert_int(shell_output_control(&output, control - 1), ==, -1);
      output.blocks[0].expanded = 1;
      view = shell_output_build(&output, text);
      munit_assert_not_null(strstr(view, "[-] 21 lines\nстрока 1\t界"));
      munit_assert_not_null(strstr(view, "строка 21\t界"));
      output.blocks[0].expanded = 0;
      view = shell_output_build(&output, text);
      munit_assert_null(strstr(view, "строка"));
    }
    munit_assert_string_equal(text, original);
    shell_output_free(&output);
  }
  return MUNIT_OK;
}

static MunitResult test_multiple_ranges(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char text[8192];
  result(text, 21);
  ShellOutput output = {0};
  size_t first_end = strlen(text);
  munit_assert_true(shell_output_add(&output, text, 0, first_end));
  strcat(text, "\nordinary [+] prose\nShell: next\n");
  size_t next = strlen(text);
  strcat(text, "[exit 0]\nsmall result\n");
  size_t next_end = strlen(text);
  munit_assert_true(shell_output_add(&output, text, next, next_end));
  strcat(text, "final answer");
  const char *view = shell_output_build(&output, text);
  size_t mapped = shell_output_offset(&output, next);
  munit_assert_string_equal(view + mapped, text + next);
  munit_assert_size(shell_output_offset(&output, next_end), ==,
                    (size_t)(strstr(view, "final answer") - view));
  munit_assert_true(shell_output_contains(&output, mapped, mapped + 8));
  munit_assert_false(shell_output_contains(&output,
      shell_output_offset(&output, next_end), strlen(view)));
  munit_assert_int(shell_output_control(&output,
      (size_t)(strstr(view, "[+] prose") - view)), ==, -1);
  const char *texts[] = {view};
  int roles[] = {1};
  linemap_build(NULL, roles, 1, texts, 8);
  size_t control = output.blocks[0].control_start;
  int found = 0;
  for (int i = 0; i < linemap_count(); i++) {
    const LineInfo *line = linemap_get(i);
    if (line->role != LINE_PADDING && (size_t)line->byte_start == control) {
      munit_assert_int(shell_output_control(&output, control), ==, 0);
      found = 1;
    }
  }
  munit_assert_true(found);
  linemap_free();
  output.blocks[0].expanded = 1;
  view = shell_output_build(&output, text);
  munit_assert_string_equal(view + shell_output_offset(&output, next), text + next);
  shell_output_free(&output);
  return MUNIT_OK;
}

static MunitResult test_manual_and_invalid(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char text[4096] = "Shell: test\n[exit 0]\n";
  for (int i = 0; i < 20; i++) strcat(text, "\n");
  strcat(text, "last"); /* Blank lines count; no trailing newline required. */
  ShellOutput output = {0};
  munit_assert_ptr_equal(shell_output_build(&output, text), text);
  munit_assert_true(shell_output_add(&output, text, strlen("Shell: test\n"), strlen(text)));
  munit_assert_false(shell_output_add(&output, text, 0, strlen(text)));
  munit_assert_false(shell_output_add(&output, text, strlen(text), strlen(text) + 10));
  const char *view = shell_output_build(&output, text);
  munit_assert_string_equal(view, "Shell: test [exit 0]\n[+] 21 lines");
  munit_assert_size(shell_output_offset(&output, strlen(text)), ==, strlen(view));
  shell_output_free(&output);
  return MUNIT_OK;
}

static MunitResult test_status_projection(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  const char *texts[] = {
    "Shell: true\n[exit 0]",
    "Shell: false\n[exit 1]\nstderr:\nfailed\n",
    "[exit 0]\nstandalone\n",
    "ordinary [+] prose\nno status\n",
  };
  const char *expected[] = {
    "Shell: true [exit 0]",
    "Shell: false [exit 1]\nstderr:\nfailed\n",
    "[exit 0]\nstandalone\n",
    "ordinary [+] prose\nno status\n",
  };
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    ShellOutput output = {0};
    munit_assert_ptr_equal(shell_output_build(&output, texts[i]), texts[i]);
    munit_assert_true(shell_output_add(&output, texts[i], 0, strlen(texts[i])));
    const char *view = shell_output_build(&output, texts[i]);
    munit_assert_string_equal(view, expected[i]);
    for (size_t p = 0; p <= strlen(texts[i]); p++) {
      size_t mapped = shell_output_offset(&output, p);
      munit_assert_size(mapped, <=, strlen(view));
      if (texts[i][p] != '\n')
        munit_assert_char(view[mapped], ==, texts[i][p]);
    }
    shell_output_free(&output);
  }
  return MUNIT_OK;
}

static MunitResult test_multiline_command_boundary(
    const MunitParameter params[], void *data) {
  (void)params; (void)data;
  const char *header = "Shell: cat <<'END'\n[exit 0]\nEND\nfalse\n";
  char text[4096];
  strcpy(text, header);
  strcat(text, "[exit 1] TIMED OUT\n");
  for (int i = 0; i < 21; i++) strcat(text, "body\n");
  ShellOutput output = {0};
  munit_assert_true(shell_output_add(&output, text, strlen(header), strlen(text)));
  const char *view = shell_output_build(&output, text);
  munit_assert_not_null(strstr(view, "[exit 0]\nEND\nfalse [exit 1] TIMED OUT\n[+] 21 lines"));
  munit_assert_null(strstr(view, "body"));
  output.blocks[0].expanded = 1;
  view = shell_output_build(&output, text);
  munit_assert_not_null(strstr(view, "[exit 1] TIMED OUT\n[-] 21 lines\nbody"));
  shell_output_free(&output);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/multiline_command_boundary", test_multiline_command_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/status_projection", test_status_projection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/threshold", test_threshold, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/multiple_ranges", test_multiple_ranges, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/manual_and_invalid", test_manual_and_invalid, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};
MunitSuite shell_output_suite = {"/shell_output", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
