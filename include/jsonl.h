#ifndef JSONL_H
#define JSONL_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  int failed;
} JsonlBuffer;

typedef ssize_t (*JsonlWriteFn)(int fd, const void *data, size_t length,
                                void *context);

void jsonl_buffer_init(JsonlBuffer *buffer);
void jsonl_buffer_free(JsonlBuffer *buffer);
int jsonl_append(JsonlBuffer *buffer, const char *text);
int jsonl_append_n(JsonlBuffer *buffer, const char *text, size_t len);
int jsonl_append_format(JsonlBuffer *buffer, const char *format, ...);
int jsonl_append_string(JsonlBuffer *buffer, const char *value);
int jsonl_write_line(int fd, const JsonlBuffer *buffer);
int jsonl_write_line_with(int fd, const JsonlBuffer *buffer,
                          JsonlWriteFn write_fn, void *context);

#endif
