#include "editor.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int editor_read_prompt_file(const char *path, char *destination, size_t capacity,
                            char *error, size_t error_capacity) {
  if (!path || !destination || capacity == 0) {
    snprintf(error, error_capacity, "Invalid prompt destination");
    return -1;
  }
  FILE *file = fopen(path, "rb");
  if (!file) {
    snprintf(error, error_capacity, "Cannot open prompt: %s", strerror(errno));
    return -1;
  }
  char *buffer = malloc(capacity);
  if (!buffer) {
    fclose(file);
    snprintf(error, error_capacity, "Cannot allocate prompt buffer");
    return -1;
  }
  errno = 0;
  size_t length = fread(buffer, 1, capacity, file);
  int read_error = ferror(file) ? (errno ? errno : EIO) : 0;
  if (fclose(file) != 0 && !read_error)
    read_error = errno ? errno : EIO;
  int result = -1;
  if (read_error) {
    snprintf(error, error_capacity, "Cannot read prompt: %s", strerror(read_error));
  } else if (length >= capacity) {
    snprintf(error, error_capacity, "Prompt exceeds %zu-byte input limit", capacity - 1);
  } else if (memchr(buffer, '\0', length)) {
    snprintf(error, error_capacity, "Prompt contains a NUL byte");
  } else {
    buffer[length] = '\0';
    memcpy(destination, buffer, length + 1);
    if (error_capacity)
      error[0] = '\0';
    result = 0;
  }
  free(buffer);
  return result;
}
