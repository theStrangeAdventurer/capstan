#include "munit.h"
#include "submission_queue.h"
#include <stdlib.h>
#include <string.h>

static MunitResult test_fifo_limit(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  SubmissionQueue queue;
  submission_queue_init(&queue);

  munit_assert_true(submission_queue_push(&queue, "one"));
  munit_assert_true(submission_queue_push(&queue, "two"));
  munit_assert_true(submission_queue_push(&queue, "three"));
  munit_assert_true(submission_queue_push(&queue, "four"));
  munit_assert_true(submission_queue_push(&queue, "five"));
  munit_assert_false(submission_queue_push(&queue, "six"));
  munit_assert_int(submission_queue_size(&queue), ==, 5);
  munit_assert_int(submission_queue_visible_size(&queue), ==, 3);
  munit_assert_string_equal(submission_queue_at(&queue, 1), "two");

  char *first = submission_queue_shift(&queue);
  munit_assert_string_equal(first, "one");
  free(first);
  munit_assert_string_equal(submission_queue_at(&queue, 0), "two");
  submission_queue_clear(&queue);
  return MUNIT_OK;
}

static MunitResult test_rejects_empty(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  SubmissionQueue queue;
  submission_queue_init(&queue);
  munit_assert_false(submission_queue_push(&queue, ""));
  munit_assert_false(submission_queue_push(&queue, NULL));
  munit_assert_int(submission_queue_size(&queue), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_shorter_item_has_no_previous_suffix(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  SubmissionQueue queue;
  submission_queue_init(&queue);

  munit_assert_true(
      submission_queue_push(&queue, "a much longer queued message"));
  munit_assert_true(submission_queue_push(&queue, "short"));
  munit_assert_string_equal(submission_queue_at(&queue, 0),
                            "a much longer queued message");
  munit_assert_string_equal(submission_queue_at(&queue, 1), "short");
  munit_assert_size(strlen(submission_queue_at(&queue, 1)), ==, 5);

  submission_queue_clear(&queue);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/fifo-limit", test_fifo_limit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects-empty", test_rejects_empty, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/shorter-item-has-no-previous-suffix",
     test_shorter_item_has_no_previous_suffix, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite submission_queue_suite = {"/submission-queue", tests, NULL, 1,
                                     MUNIT_SUITE_OPTION_NONE};
