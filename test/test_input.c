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

static MunitResult test_insert_in_middle(const MunitParameter params[],
                                         void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("+-");
  input_move_left();
  input_insert('x');
  munit_assert_string_equal(input_get_text(), "+x-");
  munit_assert_int(input_get_cursor(), ==, 2);
  return MUNIT_OK;
}

static MunitResult test_rejects_non_byte_keycode(const MunitParameter params[],
                                                 void *data) {
  (void)params;
  (void)data;
  input_init();
  input_insert('+');
  input_insert(0x101);
  input_insert('-');
  munit_assert_string_equal(input_get_text(), "+-");
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

static MunitResult test_delete_word_backward(const MunitParameter params[],
                                              void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("hello world");
  input_delete_word_backward();
  munit_assert_string_equal(input_get_text(), "hello ");
  munit_assert_int(input_get_cursor(), ==, 6);
  return MUNIT_OK;
}

static MunitResult test_delete_word_backward_in_middle(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("one twoX");
  input_move_left();
  input_delete_word_backward();
  munit_assert_string_equal(input_get_text(), "one X");
  munit_assert_int(input_get_cursor(), ==, 4);
  return MUNIT_OK;
}

static MunitResult test_delete_word_backward_utf8(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("hello мир");
  input_delete_word_backward();
  munit_assert_string_equal(input_get_text(), "hello ");
  munit_assert_int(input_get_cursor(), ==, 6);
  return MUNIT_OK;
}

static MunitResult test_delete_word_backward_unicode_punctuation(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("hello “мир”");
  input_delete_word_backward();
  munit_assert_string_equal(input_get_text(), "hello “мир");
  input_delete_word_backward();
  munit_assert_string_equal(input_get_text(), "hello “");
  return MUNIT_OK;
}

static MunitResult test_delete_to_line_start(const MunitParameter params[],
                                              void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("first\nsecond tail!");
  input_move_left();
  input_delete_to_line_start();
  munit_assert_string_equal(input_get_text(), "first\n!");
  munit_assert_int(input_get_cursor(), ==, 6);
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

static MunitResult test_image_attachment(const MunitParameter params[],
                                         void *data) {
  (void)params;
  (void)data;
  input_init();
  input_set_text("describe");
  munit_assert_true(input_add_image("image/png", "YWJj"));
  munit_assert_size(input_image_count(), ==, 1);
  munit_assert_string_equal(input_get_text(), "describe");
  munit_assert_string_equal(input_get_display_text(), "[Image 1] describe");
  munit_assert_int(input_get_display_cursor(), ==, 18);

  size_t count = 0;
  InputImage *images = input_take_images(&count);
  munit_assert_size(count, ==, 1);
  munit_assert_string_equal(images[0].mime_type, "image/png");
  munit_assert_string_equal(images[0].data, "YWJj");
  munit_assert_size(input_image_count(), ==, 0);
  input_images_free(images, count);
  return MUNIT_OK;
}

static MunitResult test_backspace_removes_image(const MunitParameter params[],
                                                void *data) {
  (void)params;
  (void)data;
  input_init();
  munit_assert_true(input_add_image("image/png", "YWJj"));
  input_backspace();
  munit_assert_size(input_image_count(), ==, 0);
  munit_assert_string_equal(input_get_display_text(), "");
  return MUNIT_OK;
}

static void paste_bytes(const char *text) {
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    munit_assert_true(input_paste_feed(*p));
}

static MunitResult test_paste_utf8_multiline(const MunitParameter params[],
                                           void *data) {
  (void)params; (void)data;
  input_init();
  munit_assert_false(input_paste_feed('x'));
  input_set_text("+-");
  input_move_left();
  input_paste_begin();
  paste_bytes("Привет\rмир\n\t/shell echo ok");
  munit_assert_string_equal(input_get_text(), "+Привет\nмир\n\t/shell echo ok-");
  munit_assert_true(input_paste_active());
  paste_bytes("\033[201~");
  munit_assert_false(input_paste_active());
  munit_assert_false(input_paste_feed('\n'));
  return MUNIT_OK;
}

static MunitResult test_paste_split_and_literal_markers(
    const MunitParameter params[], void *data) {
  (void)params; (void)data;
  input_init();
  input_paste_begin();
  paste_bytes("one\033[20");
  /* Idle frames and resize keycodes do not finish or corrupt a paste. */
  munit_assert_false(input_paste_feed(-1));
  munit_assert_false(input_paste_feed(0x101));
  munit_assert_true(input_paste_active());
  paste_bytes("x\033[A\027two\033\033[20");
  paste_bytes("1~");
  munit_assert_false(input_paste_active());
  munit_assert_string_equal(input_get_text(), "one\033[20x\033[A\027two\033");
  input_paste_begin();
  paste_bytes("next\033[201~");
  munit_assert_false(input_paste_active());
  return MUNIT_OK;
}

static MunitResult test_paste_full_buffer_still_consumes_end(
    const MunitParameter params[], void *data) {
  (void)params; (void)data;
  input_init();
  input_paste_begin();
  for (int i = 0; i < INPUT_BUFFER_SIZE + 10; i++)
    munit_assert_true(input_paste_feed('x'));
  paste_bytes("\n\033[201~");
  munit_assert_false(input_paste_active());
  munit_assert_size(strlen(input_get_text()), ==, INPUT_BUFFER_SIZE - 1);
  munit_assert_false(input_paste_feed('\n'));
  return MUNIT_OK;
}

static void complete_paste(const char *text) {
  input_paste_begin();
  paste_bytes(text);
  paste_bytes("\033[201~");
}

static MunitResult test_collapsed_paste(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  input_init();
  input_set_paste_width(4);
  complete_paste("12345678");
  munit_assert_string_equal(input_get_display_text(), "12345678");
  input_clear();
  input_set_text("+-");
  input_move_left();
  input_paste_begin();
  paste_bytes("123456789");
  munit_assert_string_equal(input_get_display_text(), "+123456789-");
  paste_bytes("\033[201~");
  munit_assert_string_equal(input_get_text(), "+123456789-");
  munit_assert_string_equal(input_get_display_text(), "+[Pasted 9 chars]-");
  munit_assert_int(input_get_display_cursor(), ==, 17);
  input_move_left();
  munit_assert_int(input_get_cursor(), ==, 1);
  munit_assert_int(input_get_display_cursor(), ==, 1);
  input_insert('x');
  input_move_right();
  munit_assert_int(input_get_cursor(), ==, 11);
  input_insert('y');
  input_backspace();
  input_backspace();
  munit_assert_string_equal(input_get_text(), "+x-");
  munit_assert_string_equal(input_get_display_text(), "+x-");
  return MUNIT_OK;
}

static MunitResult test_multiple_paste_blocks(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  input_init();
  input_set_paste_width(4);
  complete_paste("123456789");
  complete_paste("abcdefghi");
  input_move_left();
  complete_paste("ABCDEFGHI");
  munit_assert_string_equal(input_get_text(), "123456789ABCDEFGHIabcdefghi");
  munit_assert_string_equal(input_get_display_text(),
      "[Pasted 9 chars][Pasted 9 chars][Pasted 9 chars]");
  munit_assert_int(input_get_display_cursor(), ==, 32);
  input_backspace();
  munit_assert_string_equal(input_get_text(), "123456789abcdefghi");
  input_move_left();
  input_insert('!');
  input_backspace();
  input_move_right();
  input_backspace();
  munit_assert_string_equal(input_get_text(), "abcdefghi");
  munit_assert_string_equal(input_get_display_text(), "[Pasted 9 chars]");
  input_move_right();
  input_backspace();
  munit_assert_string_equal(input_get_text(), "");
  return MUNIT_OK;
}

static MunitResult test_paste_unicode_and_deletions(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  input_init();
  input_set_paste_width(80);
  input_set_text("prefix ");
  complete_paste("я\n猫\n😀");
  munit_assert_string_equal(input_get_text(), "prefix я\n猫\n😀");
  munit_assert_string_equal(input_get_display_text(), "prefix [Pasted 5 chars]");
  munit_assert_string_equal(input_get_message_text(), "prefix я\n猫\n😀");
  munit_assert_string_equal(input_get_display_text(), "prefix [Pasted 5 chars]");
  munit_assert_true(input_add_image("image/png", "YWJj"));
  munit_assert_string_equal(input_get_message_text(), "[Image 1] prefix я\n猫\n😀");
  munit_assert_string_equal(input_get_display_text(), "[Image 1] prefix [Pasted 5 chars]");
  munit_assert_int(input_get_display_cursor(), ==, (int)strlen(input_get_display_text()));
  input_delete_to_line_start();
  munit_assert_string_equal(input_get_text(), "prefix ");
  complete_paste("a\nb\n");
  input_delete_to_line_start();
  munit_assert_string_equal(input_get_text(), "prefix ");
  complete_paste("a\nb\n\n");
  input_delete_to_line_start();
  munit_assert_string_equal(input_get_text(), "prefix ");
  input_insert('\n');
  input_delete_to_line_start();
  munit_assert_string_equal(input_get_text(), "prefix \n");
  input_backspace();
  complete_paste("a\nb\nc");
  input_delete_word_backward();
  munit_assert_string_equal(input_get_text(), "prefix ");
  complete_paste("a\nb\nc");
  input_set_text("a\nb\nc");
  munit_assert_string_equal(input_get_display_text(), "[Image 1] a\nb\nc");
  input_clear();
  munit_assert_string_equal(input_get_display_text(), "");
  complete_paste("a\nb\nc");
  input_init();
  munit_assert_string_equal(input_get_display_text(), "");
  return MUNIT_OK;
}

static MunitResult test_paste_display_capacity(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  input_init();
  input_set_paste_width(1);
  for (int i = 0; i < (INPUT_BUFFER_SIZE - 1) / 3; i++)
    complete_paste("abc");
  size_t blocks = (INPUT_BUFFER_SIZE - 1) / 3;
  munit_assert_size(strlen(input_get_text()), ==, blocks * 3);
  munit_assert_size(strlen(input_get_display_text()), ==, blocks * 16);
  munit_assert_int(input_get_display_cursor(), ==, (int)(blocks * 16));
  input_clear();
  complete_paste("a\n");
  munit_assert_string_equal(input_get_display_text(), "a\n");
  input_clear();
  complete_paste("a\n\n");
  munit_assert_string_equal(input_get_display_text(), "[Pasted 3 chars]");
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/collapsed_paste", test_collapsed_paste, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/multiple_paste_blocks", test_multiple_paste_blocks, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/paste_unicode_and_deletions", test_paste_unicode_and_deletions, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/paste_display_capacity", test_paste_display_capacity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/paste_utf8_multiline", test_paste_utf8_multiline, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/paste_split_and_literal_markers", test_paste_split_and_literal_markers,
   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/paste_full_buffer_still_consumes_end", test_paste_full_buffer_still_consumes_end,
   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/init", test_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_ascii", test_insert_ascii, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_in_middle", test_insert_in_middle, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/rejects_non_byte_keycode", test_rejects_non_byte_keycode, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_newline", test_insert_newline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_empty", test_backspace_empty, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_single", test_backspace_single, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_middle", test_backspace_middle, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/delete_word_backward", test_delete_word_backward, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/delete_word_backward_in_middle", test_delete_word_backward_in_middle,
   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/delete_word_backward_utf8", test_delete_word_backward_utf8, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/delete_word_backward_unicode_punctuation",
   test_delete_word_backward_unicode_punctuation, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/delete_to_line_start", test_delete_to_line_start, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/clear", test_clear, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/move_left_boundary", test_move_left_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/move_right_boundary", test_move_right_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/move_left_right_ascii", test_move_left_right_ascii, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/utf8_insert", test_utf8_insert, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/utf8_move_left", test_utf8_move_left, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/utf8_backspace", test_utf8_backspace, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/insert_clamps_at_buffer_end", test_insert_clamps_at_buffer_end, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/image_attachment", test_image_attachment, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/backspace_removes_image", test_backspace_removes_image, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite input_suite = {"/input", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
