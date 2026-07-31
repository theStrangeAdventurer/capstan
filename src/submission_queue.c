#include "submission_queue.h"
#include "utils.h"
#include <stdlib.h>

void submission_queue_init(SubmissionQueue *queue) {
  if (!queue)
    return;
  for (int i = 0; i < SUBMISSION_QUEUE_LIMIT; i++)
    queue->items[i] = NULL;
  queue->size = 0;
}

int submission_queue_push(SubmissionQueue *queue, const char *text) {
  if (!queue || !text || !text[0] || queue->size >= SUBMISSION_QUEUE_LIMIT)
    return 0;
  char *copy = my_strdup(text);
  if (!copy)
    return 0;
  queue->items[queue->size++] = copy;
  return 1;
}

const char *submission_queue_at(const SubmissionQueue *queue, int index) {
  if (!queue || index < 0 || index >= queue->size)
    return NULL;
  return queue->items[index];
}

char *submission_queue_shift(SubmissionQueue *queue) {
  if (!queue || queue->size <= 0)
    return NULL;
  char *item = queue->items[0];
  for (int i = 1; i < queue->size; i++)
    queue->items[i - 1] = queue->items[i];
  queue->size--;
  queue->items[queue->size] = NULL;
  return item;
}

int submission_queue_size(const SubmissionQueue *queue) {
  return queue ? queue->size : 0;
}

int submission_queue_visible_size(const SubmissionQueue *queue) {
  int size = submission_queue_size(queue);
  return size < SUBMISSION_QUEUE_VISIBLE_LIMIT
             ? size
             : SUBMISSION_QUEUE_VISIBLE_LIMIT;
}

void submission_queue_clear(SubmissionQueue *queue) {
  if (!queue)
    return;
  for (int i = 0; i < queue->size; i++)
    free(queue->items[i]);
  submission_queue_init(queue);
}
