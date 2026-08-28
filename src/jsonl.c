#include "jsonl.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int reserve(JsonlBuffer *buffer, size_t extra) {
  if (!buffer || buffer->failed || extra > SIZE_MAX - buffer->len - 1) {
    if (buffer)
      buffer->failed = 1;
    return 0;
  }
  size_t needed = buffer->len + extra + 1;
  if (needed <= buffer->cap)
    return 1;
  size_t next = buffer->cap ? buffer->cap : 256;
  while (next < needed) {
    if (next > SIZE_MAX / 2) {
      buffer->failed = 1;
      return 0;
    }
    next *= 2;
  }
  char *grown = realloc(buffer->data, next);
  if (!grown) {
    buffer->failed = 1;
    return 0;
  }
  buffer->data = grown;
  buffer->cap = next;
  return 1;
}

void jsonl_buffer_init(JsonlBuffer *buffer) {
  if (buffer)
    memset(buffer, 0, sizeof(*buffer));
}

void jsonl_buffer_free(JsonlBuffer *buffer) {
  if (!buffer)
    return;
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

int jsonl_append_n(JsonlBuffer *buffer, const char *text, size_t len) {
  if ((!text && len > 0) || !reserve(buffer, len))
    return 0;
  if (len > 0)
    memcpy(buffer->data + buffer->len, text, len);
  buffer->len += len;
  buffer->data[buffer->len] = '\0';
  return 1;
}

int jsonl_append(JsonlBuffer *buffer, const char *text) {
  return jsonl_append_n(buffer, text ? text : "", text ? strlen(text) : 0);
}

int jsonl_append_format(JsonlBuffer *buffer, const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (needed < 0 || !reserve(buffer, (size_t)needed)) {
    va_end(args);
    return 0;
  }
  vsnprintf(buffer->data + buffer->len, buffer->cap - buffer->len, format,
            args);
  va_end(args);
  buffer->len += (size_t)needed;
  return 1;
}

static size_t valid_utf8_length(const unsigned char *p, size_t remaining) {
  if (remaining == 0)
    return 0;
  if (p[0] < 0x80)
    return 1;
  if (remaining >= 2 && p[0] >= 0xc2 && p[0] <= 0xdf &&
      p[1] >= 0x80 && p[1] <= 0xbf)
    return 2;
  if (remaining >= 3 && p[0] == 0xe0 && p[1] >= 0xa0 && p[1] <= 0xbf &&
      p[2] >= 0x80 && p[2] <= 0xbf)
    return 3;
  if (remaining >= 3 &&
      ((p[0] >= 0xe1 && p[0] <= 0xec) ||
       (p[0] >= 0xee && p[0] <= 0xef)) &&
      p[1] >= 0x80 && p[1] <= 0xbf && p[2] >= 0x80 && p[2] <= 0xbf)
    return 3;
  if (remaining >= 3 && p[0] == 0xed && p[1] >= 0x80 && p[1] <= 0x9f &&
      p[2] >= 0x80 && p[2] <= 0xbf)
    return 3;
  if (remaining >= 4 && p[0] == 0xf0 && p[1] >= 0x90 && p[1] <= 0xbf &&
      p[2] >= 0x80 && p[2] <= 0xbf && p[3] >= 0x80 && p[3] <= 0xbf)
    return 4;
  if (remaining >= 4 && p[0] >= 0xf1 && p[0] <= 0xf3 &&
      p[1] >= 0x80 && p[1] <= 0xbf && p[2] >= 0x80 && p[2] <= 0xbf &&
      p[3] >= 0x80 && p[3] <= 0xbf)
    return 4;
  if (remaining >= 4 && p[0] == 0xf4 && p[1] >= 0x80 && p[1] <= 0x8f &&
      p[2] >= 0x80 && p[2] <= 0xbf && p[3] >= 0x80 && p[3] <= 0xbf)
    return 4;
  return 0;
}

int jsonl_append_string(JsonlBuffer *buffer, const char *value) {
  const unsigned char *p = (const unsigned char *)(value ? value : "");
  size_t remaining = strlen((const char *)p);
  size_t original_len = buffer ? buffer->len : 0;
  if (!jsonl_append(buffer, "\""))
    return 0;
  while (remaining > 0) {
    int ok = 1;
    size_t consumed = 1;
    switch (*p) {
    case '"':
      ok = jsonl_append(buffer, "\\\"");
      break;
    case '\\':
      ok = jsonl_append(buffer, "\\\\");
      break;
    case '\b':
      ok = jsonl_append(buffer, "\\b");
      break;
    case '\f':
      ok = jsonl_append(buffer, "\\f");
      break;
    case '\n':
      ok = jsonl_append(buffer, "\\n");
      break;
    case '\r':
      ok = jsonl_append(buffer, "\\r");
      break;
    case '\t':
      ok = jsonl_append(buffer, "\\t");
      break;
    default:
      if (*p < 0x20) {
        ok = jsonl_append_format(buffer, "\\u%04x", (unsigned int)*p);
      } else {
        size_t length = valid_utf8_length(p, remaining);
        if (length == 0) {
          ok = jsonl_append(buffer, "\\ufffd");
        } else {
          ok = jsonl_append_n(buffer, (const char *)p, length);
          consumed = length;
        }
      }
      break;
    }
    if (!ok)
      goto fail;
    p += consumed;
    remaining -= consumed;
  }
  if (!jsonl_append(buffer, "\""))
    goto fail;
  return 1;

fail:
  if (buffer && buffer->data && original_len < buffer->cap) {
    buffer->len = original_len;
    buffer->data[original_len] = '\0';
  }
  return 0;
}

static ssize_t system_write(int fd, const void *data, size_t length,
                            void *context) {
  (void)context;
  return write(fd, data, length);
}

int jsonl_write_line_with(int fd, const JsonlBuffer *buffer,
                          JsonlWriteFn write_fn, void *context) {
  if (fd < 0 || !buffer || buffer->failed || !write_fn ||
      buffer->len == SIZE_MAX) {
    errno = EINVAL;
    return 0;
  }

  off_t original_end = lseek(fd, 0, SEEK_END);
  if (original_end < 0)
    return 0;

  char *line = malloc(buffer->len + 1);
  if (!line)
    return 0;
  if (buffer->len > 0)
    memcpy(line, buffer->data, buffer->len);
  line[buffer->len] = '\n';

  size_t offset = 0;
  size_t length = buffer->len + 1;
  while (offset < length) {
    ssize_t count = write_fn(fd, line + offset, length - offset, context);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      int saved = count == 0 ? EIO : errno;
      if (ftruncate(fd, original_end) != 0)
        saved = errno;
      (void)lseek(fd, original_end, SEEK_SET);
      free(line);
      errno = saved;
      return 0;
    }
    offset += (size_t)count;
  }
  free(line);
  return 1;
}

int jsonl_write_line(int fd, const JsonlBuffer *buffer) {
  return jsonl_write_line_with(fd, buffer, system_write, NULL);
}
