#ifndef SUBMISSION_QUEUE_H
#define SUBMISSION_QUEUE_H

#define SUBMISSION_QUEUE_LIMIT 5
#define SUBMISSION_QUEUE_VISIBLE_LIMIT 3

typedef struct {
  char *items[SUBMISSION_QUEUE_LIMIT];
  int size;
} SubmissionQueue;

void submission_queue_init(SubmissionQueue *queue);
int submission_queue_push(SubmissionQueue *queue, const char *text);
const char *submission_queue_at(const SubmissionQueue *queue, int index);
char *submission_queue_shift(SubmissionQueue *queue);
int submission_queue_size(const SubmissionQueue *queue);
int submission_queue_visible_size(const SubmissionQueue *queue);
void submission_queue_clear(SubmissionQueue *queue);

#endif
