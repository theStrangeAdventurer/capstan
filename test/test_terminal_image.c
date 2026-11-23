#include "munit.h"
#include "linemap.h"
#include "terminal_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIXTURE "test/fixtures/vision-shapes.png"

static MunitResult test_detects_plain_path(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[512];
  munit_assert_int(terminal_image_line(FIXTURE, strlen(FIXTURE), path, sizeof(path)), ==, 1);
  munit_assert_string_equal(path, FIXTURE);
  return MUNIT_OK;
}

static MunitResult test_detects_wrapped_paths(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *lines[] = {
      "- " FIXTURE,
      "* `" FIXTURE "`",
      "  > \"" FIXTURE "\"",
      "1. " FIXTURE ",",
      "   " FIXTURE,
  };
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    char path[512];
    munit_assert_int(terminal_image_line(lines[i], strlen(lines[i]), path, sizeof(path)),
                     ==, 1);
    munit_assert_string_equal(path, FIXTURE);
  }
  return MUNIT_OK;
}

static MunitResult test_rejects_non_image_lines(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *lines[] = {
      "",
      "plain prose without a path",
      "see " FIXTURE " for the screenshot",
      "test/fixtures/vision-shapes.txt",
      "test/fixtures/missing.png",
      "src/tui.c",
      "https://example.com/shot.png",
  };
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    char path[512];
    munit_assert_int(terminal_image_line(lines[i], strlen(lines[i]), path, sizeof(path)),
                     ==, 0);
  }
  return MUNIT_OK;
}

static MunitResult test_rejects_extension_without_magic(const MunitParameter params[],
                                                       void *data) {
  (void)params;
  (void)data;
  char name[128];
  snprintf(name, sizeof(name), "build/test-not-an-image-%d.png", (int)getpid());
  FILE *file = fopen(name, "wb");
  munit_assert_not_null(file);
  fputs("this is not a png", file);
  fclose(file);
  char path[512];
  int detected = terminal_image_line(name, strlen(name), path, sizeof(path));
  int sized = terminal_image_size(name, &(int){0}, &(int){0});
  remove(name);
  munit_assert_int(detected, ==, 0);
  munit_assert_int(sized, ==, 0);
  return MUNIT_OK;
}

static void write_gif(const char *name, int width, int height) {
  unsigned char gif[32] = {0};
  memcpy(gif, "GIF89a", 6);
  gif[6] = (unsigned char)(width & 0xff);
  gif[7] = (unsigned char)(width >> 8);
  gif[8] = (unsigned char)(height & 0xff);
  gif[9] = (unsigned char)(height >> 8);
  FILE *file = fopen(name, "wb");
  if (!file) return;
  fwrite(gif, 1, sizeof(gif), file);
  fclose(file);
}

static MunitResult test_clipboard_mime_type(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char mime[32];
  char name[128];
  snprintf(name, sizeof(name), "build/test-mime-%d.gif", (int)getpid());
  write_gif(name, 32, 24);
  terminal_image_reset();
  munit_assert_int(terminal_image_mime(FIXTURE, mime, sizeof(mime)), ==, 1);
  munit_assert_string_equal(mime, "image/png");
  munit_assert_int(terminal_image_mime(name, mime, sizeof(mime)), ==, 1);
  munit_assert_string_equal(mime, "image/gif");
  munit_assert_int(terminal_image_mime(FIXTURE, mime, 4), ==, 0);
  munit_assert_int(terminal_image_mime("src/tui.c", mime, sizeof(mime)), ==, 0);
  terminal_image_reset();
  remove(name);
  return MUNIT_OK;
}

static MunitResult test_reads_png_dimensions(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int width = 0, height = 0;
  terminal_image_reset();
  munit_assert_int(terminal_image_size(FIXTURE, &width, &height), ==, 1);
  munit_assert_int(width, ==, 1254);
  munit_assert_int(height, ==, 1254);
  terminal_image_reset();
  return MUNIT_OK;
}

static MunitResult test_reads_gif_dimensions(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char name[128];
  snprintf(name, sizeof(name), "build/test-image-%d.gif", (int)getpid());
  unsigned char gif[32] = {0};
  memcpy(gif, "GIF89a", 6);
  gif[6] = 0x40; /* 320 wide, little endian. */
  gif[7] = 0x01;
  gif[8] = 0xf0; /* 240 high. */
  gif[9] = 0x00;
  FILE *file = fopen(name, "wb");
  munit_assert_not_null(file);
  fwrite(gif, 1, sizeof(gif), file);
  fclose(file);
  int width = 0, height = 0;
  terminal_image_reset();
  int ok = terminal_image_size(name, &width, &height);
  terminal_image_reset();
  remove(name);
  munit_assert_int(ok, ==, 1);
  munit_assert_int(width, ==, 320);
  munit_assert_int(height, ==, 240);
  return MUNIT_OK;
}

static MunitResult test_box_clamps_to_640(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  TerminalImageBox box;
  munit_assert_int(terminal_image_box(80, 0, 8, 16, 1254, 1254, &box), ==, 1);
  munit_assert_int(box.width_px, ==, TERMINAL_IMAGE_MAX_PX);
  munit_assert_int(box.cols, ==, 80);
  munit_assert_int(box.rows, ==, 40);
  return MUNIT_OK;
}

static MunitResult test_box_follows_aspect(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  TerminalImageBox box;
  munit_assert_int(terminal_image_box(80, 0, 8, 16, 1000, 250, &box), ==, 1);
  munit_assert_int(box.width_px, ==, 640);
  munit_assert_int(box.rows, ==, 10);
  return MUNIT_OK;
}

static MunitResult test_box_shrinks_to_pane(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  TerminalImageBox box;
  munit_assert_int(terminal_image_box(30, 0, 8, 16, 1254, 1254, &box), ==, 1);
  munit_assert_int(box.width_px, ==, 240);
  munit_assert_int(box.cols, ==, 30);
  munit_assert_int(box.rows, ==, 15);
  munit_assert_int(terminal_image_box(0, 0, 8, 16, 1254, 1254, &box), ==, 0);
  munit_assert_int(terminal_image_box(80, 0, 8, 16, 0, 0, &box), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_box_fits_pane_height(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  TerminalImageBox box;
  /* A square 640 px image is 40 rows; a 20-row pane shrinks it to 40x20 cells. */
  munit_assert_int(terminal_image_box(80, 20, 8, 16, 1254, 1254, &box), ==, 1);
  munit_assert_int(box.rows, ==, 20);
  munit_assert_int(box.cols, ==, 40);
  munit_assert_int(box.width_px, ==, 320);
  return MUNIT_OK;
}

static long drain(FILE *stream, char *buffer, size_t capacity) {
  fflush(stream);
  rewind(stream);
  size_t got = fread(buffer, 1, capacity - 1, stream);
  buffer[got] = '\0';
  return (long)got;
}

static MunitResult test_draw_emits_kitty_graphics(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char name[128];
  snprintf(name, sizeof(name), "build/test-draw-%d.gif", (int)getpid());
  unsigned char gif[32] = {0};
  memcpy(gif, "GIF89a", 6);
  gif[6] = 0x40;
  gif[7] = 0x01;
  gif[8] = 0xf0;
  FILE *source = fopen(name, "wb");
  munit_assert_not_null(source);
  fwrite(gif, 1, sizeof(gif), source);
  fclose(source);

  terminal_image_reset();
  TerminalImage item = {.row = 5, .col = 3, .path = name,
                        .box = {.cols = 40, .rows = 15, .width_px = 320}};
  FILE *stream = tmpfile();
  munit_assert_not_null(stream);
  char buffer[2048];

  terminal_image_draw(stream, &item, 1);
  long length = drain(stream, buffer, sizeof(buffer));
  munit_assert_true(length > 0);
  const char *expected = "\033_Ga=d,d=A,q=2;\033\\\0337\033[5;3H"
                         "\033_Ga=T,f=100,c=40,r=15,q=2,m=0;";
  munit_assert_int(strncmp(buffer, expected, strlen(expected)), ==, 0);
  munit_assert_not_null(strstr(buffer, "R0lGODlhQAHw")); /* 320x240 GIF header. */
  munit_assert_not_null(strstr(buffer, "\0338"));

  FILE *again = tmpfile();
  munit_assert_not_null(again);
  terminal_image_draw(again, &item, 1);
  munit_assert_true(drain(again, buffer, sizeof(buffer)) == 0);

  terminal_image_draw(again, NULL, 0);
  munit_assert_true(drain(again, buffer, sizeof(buffer)) > 0);
  munit_assert_string_equal(buffer, "\033_Ga=d,d=A,q=2;\033\\");

  fclose(again);
  fclose(stream);
  remove(name);
  terminal_image_reset();
  return MUNIT_OK;
}

static int reserve_two_rows(const char *line, size_t length) {
  return length >= 3 && strncmp(line, "IMG", 3) == 0 ? 2 : 0;
}

static MunitResult test_linemap_reserves_image_rows(const MunitParameter params[],
                                                    void *data) {
  (void)params;
  (void)data;
  const char *texts[] = {"text\nIMG shot.png\ntail"};
  int roles[] = {0};
  linemap_set_image_rows(reserve_two_rows);
  linemap_build(NULL, roles, 1, texts, 80);
  munit_assert_int(linemap_count(), ==, 10);
  munit_assert_int(linemap_get(0)->role, ==, LINE_PADDING);
  munit_assert_int(linemap_get(1)->role, ==, 0);
  const LineInfo *anchor = linemap_get(2);
  munit_assert_int(anchor->role, ==, 0);
  munit_assert_int(anchor->byte_start, ==, 5);
  /* Two spacer rows keep the picture off the path text. */
  for (int i = 3; i <= 4; i++) {
    munit_assert_int(linemap_get(i)->role, ==, LINE_IMAGE);
    munit_assert_int(linemap_get(i)->char_count, ==, 0);
  }
  const LineInfo *first = linemap_get(5);
  munit_assert_int(first->role, ==, LINE_IMAGE);
  munit_assert_int(first->char_count, ==, 2);
  munit_assert_int(first->byte_start, ==, 5);
  const LineInfo *second = linemap_get(6);
  munit_assert_int(second->role, ==, LINE_IMAGE);
  munit_assert_int(second->char_count, ==, 0);
  /* The caption row sits below the picture. */
  munit_assert_int(linemap_get(7)->role, ==, LINE_IMAGE);
  munit_assert_int(linemap_get(7)->char_count, ==, LINE_IMAGE_CAPTION);
  munit_assert_int(linemap_get(8)->role, ==, 0);
  munit_assert_int(linemap_get(9)->role, ==, LINE_PADDING);
  linemap_set_image_rows(NULL);
  linemap_free();
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/detects_plain_path", test_detects_plain_path, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/detects_wrapped_paths", test_detects_wrapped_paths, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_non_image_lines", test_rejects_non_image_lines, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_extension_without_magic", test_rejects_extension_without_magic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/clipboard_mime_type", test_clipboard_mime_type, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/reads_png_dimensions", test_reads_png_dimensions, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/reads_gif_dimensions", test_reads_gif_dimensions, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/box_clamps_to_640", test_box_clamps_to_640, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/box_follows_aspect", test_box_follows_aspect, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/box_shrinks_to_pane", test_box_shrinks_to_pane, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/box_fits_pane_height", test_box_fits_pane_height, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/draw_emits_kitty_graphics", test_draw_emits_kitty_graphics, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/linemap_reserves_image_rows", test_linemap_reserves_image_rows, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite terminal_image_suite = {"/terminal_image", tests, NULL, 1,
                                   MUNIT_SUITE_OPTION_NONE};
