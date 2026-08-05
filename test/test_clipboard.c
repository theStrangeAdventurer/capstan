#include "clipboard.h"
#include "munit.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static MunitResult test_base64_vectors(const MunitParameter params[],
                                       void *data) {
  (void)params;
  (void)data;
  static const unsigned char one[] = {'f'};
  static const unsigned char two[] = {'f', 'o'};
  static const unsigned char three[] = {'f', 'o', 'o'};

  char *encoded = clipboard_base64_encode(NULL, 0);
  munit_assert_not_null(encoded);
  munit_assert_string_equal(encoded, "");
  free(encoded);

  encoded = clipboard_base64_encode(one, sizeof(one));
  munit_assert_string_equal(encoded, "Zg==");
  free(encoded);

  encoded = clipboard_base64_encode(two, sizeof(two));
  munit_assert_string_equal(encoded, "Zm8=");
  free(encoded);

  encoded = clipboard_base64_encode(three, sizeof(three));
  munit_assert_string_equal(encoded, "Zm9v");
  free(encoded);
  return MUNIT_OK;
}

static MunitResult test_base64_rejects_null_data(const MunitParameter params[],
                                                  void *data) {
  (void)params;
  (void)data;
  munit_assert_null(clipboard_base64_encode(NULL, 1));
  return MUNIT_OK;
}

static MunitResult test_limited_reader_exact_limit(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int pipefd[2];
  munit_assert_int(pipe(pipefd), ==, 0);
  munit_assert_int((int)write(pipefd[1], "test", 4), ==, 4);
  close(pipefd[1]);

  unsigned char *result = NULL;
  size_t size = 0;
  munit_assert_int(clipboard_read_fd_limited(pipefd[0], 4, &result, &size),
                   ==, CLIPBOARD_READ_OK);
  close(pipefd[0]);
  munit_assert_size(size, ==, 4);
  munit_assert_memory_equal(4, result, "test");
  free(result);
  return MUNIT_OK;
}

static MunitResult test_limited_reader_rejects_overflow(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  int pipefd[2];
  munit_assert_int(pipe(pipefd), ==, 0);
  munit_assert_int((int)write(pipefd[1], "tests", 5), ==, 5);
  close(pipefd[1]);

  unsigned char *result = NULL;
  size_t size = 0;
  munit_assert_int(clipboard_read_fd_limited(pipefd[0], 4, &result, &size),
                   ==, CLIPBOARD_READ_OVERFLOW);
  close(pipefd[0]);
  munit_assert_null(result);
  munit_assert_size(size, ==, 0);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/base64_vectors", test_base64_vectors, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/base64_rejects_null_data", test_base64_rejects_null_data, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/limited_reader_exact_limit", test_limited_reader_exact_limit, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/limited_reader_rejects_overflow", test_limited_reader_rejects_overflow,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

MunitSuite clipboard_suite = {"/clipboard", tests, NULL, 1,
                              MUNIT_SUITE_OPTION_NONE};
