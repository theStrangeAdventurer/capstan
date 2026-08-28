#include "jsonl.h"
#include "munit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  int calls;
} FailingWriter;

static ssize_t write_then_fail(int fd, const void *data, size_t length,
                               void *context) {
  FailingWriter *writer = context;
  writer->calls++;
  if (writer->calls == 1) {
    size_t first = length > 3 ? 3 : length;
    return write(fd, data, first);
  }
  errno = EIO;
  return -1;
}

static MunitResult test_escapes_controls_and_truncated_utf8(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  JsonlBuffer buffer;
  jsonl_buffer_init(&buffer);
  const char value[] = {'a', '\n', '\1', (char)0xe2, (char)0x82, '\0'};
  munit_assert_true(jsonl_append_string(&buffer, value));
  munit_assert_string_equal(buffer.data, "\"a\\n\\u0001\\ufffd\\ufffd\"");
  munit_assert_false(buffer.failed);
  jsonl_buffer_free(&buffer);
  return MUNIT_OK;
}

static MunitResult test_rolls_back_partial_write(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-jsonl-rollback-%ld",
           (long)getpid());
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  munit_assert_int(fd, >=, 0);
  munit_assert_int((int)write(fd, "prefix\n", 7), ==, 7);

  JsonlBuffer buffer;
  jsonl_buffer_init(&buffer);
  munit_assert_true(jsonl_append(&buffer, "{\"ok\":true}"));
  FailingWriter writer = {0};
  munit_assert_false(jsonl_write_line_with(fd, &buffer, write_then_fail,
                                           &writer));
  munit_assert_int(errno, ==, EIO);

  munit_assert_int((int)lseek(fd, 0, SEEK_SET), ==, 0);
  char content[64] = "";
  munit_assert_int((int)read(fd, content, sizeof(content) - 1), ==, 7);
  munit_assert_string_equal(content, "prefix\n");

  munit_assert_true(jsonl_write_line(fd, &buffer));
  munit_assert_int((int)lseek(fd, 0, SEEK_SET), ==, 0);
  memset(content, 0, sizeof(content));
  munit_assert_int((int)read(fd, content, sizeof(content) - 1), >, 7);
  munit_assert_string_equal(content, "prefix\n{\"ok\":true}\n");
  jsonl_buffer_free(&buffer);
  close(fd);
  unlink(path);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/escapes_controls_and_truncated_utf8",
     test_escapes_controls_and_truncated_utf8, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/rolls_back_partial_write", test_rolls_back_partial_write, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite jsonl_suite = {"/jsonl", tests, NULL, 1,
                          MUNIT_SUITE_OPTION_NONE};
