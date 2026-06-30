#include "munit.h"
#include "input.h"
#include <string.h>

static MunitResult test_init(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  munit_assert_string_equal(input_get_text(), "");
  munit_assert_int(input_get_cursor(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_insert_ascii(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('h');
  input_insert('i');
  munit_assert_string_equal(input_get_text(), "hi");
  munit_assert_int(input_get_cursor(), ==, 2);
  return MUNIT_OK;
}

static MunitResult test_insert_newline(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('a');
  input_insert('\n');
  input_insert('b');
  munit_assert_string_equal(input_get_text(), "a\nb");
  munit_assert_int(input_get_cursor(), ==, 3);
  return MUNIT_OK;
}

static MunitResult test_backspace_empty(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_backspace();
  munit_assert_string_equal(input_get_text(), "");
  munit_assert_int(input_get_cursor(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_backspace_single(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('a');
  input_backspace();
  munit_assert_string_equal(input_get_text(), "");
  munit_assert_int(input_get_cursor(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_backspace_middle(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('a');
  input_insert('b');
  input_insert('c');
  input_move_left();
  input_backspace();
  munit_assert_string_equal(input_get_text(), "ac");
  munit_assert_int(input_get_cursor(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_clear(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('h');
  input_insert('e');
  input_insert('l');
  input_insert('l');
  input_insert('o');
  input_clear();
  munit_assert_string_equal(input_get_text(), "");
  munit_assert_int(input_get_cursor(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_move_left_boundary(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_move_left();
  munit_assert_int(input_get_cursor(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_move_right_boundary(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('a');
  input_move_left();
  input_move_right();
  input_move_right();
  munit_assert_int(input_get_cursor(), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_move_left_right_ascii(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('a');
  input_insert('b');
  input_insert('c');
  munit_assert_int(input_get_cursor(), ==, 3);
  input_move_left();
  munit_assert_int(input_get_cursor(), ==, 2);
  input_move_left();
  munit_assert_int(input_get_cursor(), ==, 1);
  input_move_right();
  munit_assert_int(input_get_cursor(), ==, 2);
  return MUNIT_OK;
}

static MunitResult test_utf8_insert(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert(0xD0);
  input_insert(0xB0);
  munit_assert_int(input_get_cursor(), ==, 2);
  input_insert('x');
  munit_assert_int(input_get_cursor(), ==, 3);
  return MUNIT_OK;
}

static MunitResult test_utf8_move_left(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert(0xD0);
  input_insert(0xB0);
  input_insert('x');
  munit_assert_int(input_get_cursor(), ==, 3);
  input_move_left();
  munit_assert_int(input_get_cursor(), ==, 2);
  input_move_left();
  munit_assert_int(input_get_cursor(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_utf8_backspace(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert(0xD0);
  input_insert(0xB0);
  input_insert('x');
  input_backspace();
  munit_assert_string_equal(input_get_text(), "\xD0\xB0");
  munit_assert_int(input_get_cursor(), ==, 2);
  return MUNIT_OK;
}

static MunitResult test_insert_clamps_at_buffer_end(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  for (int i = 0; i < INPUT_BUFFER_SIZE + 10; i++)
    input_insert('x');
  munit_assert_int(input_get_cursor(), ==, INPUT_BUFFER_SIZE - 1);
  munit_assert_int((int)strlen(input_get_text()), ==, INPUT_BUFFER_SIZE - 1);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/init", test_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_ascii", test_insert_ascii, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_newline", test_insert_newline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_empty", test_backspace_empty, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_single", test_backspace_single, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_middle", test_backspace_middle, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/clear", test_clear, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/move_left_boundary", test_move_left_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/move_right_boundary", test_move_right_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/move_left_right_ascii", test_move_left_right_ascii, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/utf8_insert", test_utf8_insert, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/utf8_move_left", test_utf8_move_left, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/utf8_backspace", test_utf8_backspace, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_clamps_at_buffer_end", test_insert_clamps_at_buffer_end, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite input_suite = {"/input", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
