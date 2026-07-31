#include "utils.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char *my_strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *new = malloc(len);
  if (new == NULL)
    return NULL;
  return memcpy(new, s, len);
}

int replace_with(char *input, size_t input_size, const char *from,
                 const char *to) {
  if (input == NULL || from == NULL || to == NULL || input_size == 0 ||
      from[0] == '\0') {
    return -1;
  }

  char *input_end = memchr(input, '\0', input_size);
  if (!input_end)
    return -1;

  char *p = strstr(input, from);

  if (!p) {
    return 0;
  }

  size_t prefix_len = (size_t)(p - input);
  size_t from_len = strlen(from);
  size_t to_len = strlen(to);
  size_t suffix_len = strlen(p + from_len);
  if (prefix_len + to_len + suffix_len + 1 > input_size)
    return -1;

  memmove(input + prefix_len + to_len, p + from_len, suffix_len + 1);
  memcpy(input + prefix_len, to, to_len);

  return 1;
}

int get_prev_char_start(const char *str, int pos) {
  if (pos <= 0)
    return pos;
  pos--;
  while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
    pos--;
  }
  return pos;
}

int count_visible_chars(const char *str, int byte_pos) {
  int chars = 0;
  for (int i = 0; i < byte_pos && str[i]; i++) {
    if ((str[i] & 0xC0) != 0x80)
      chars++;
  }
  return chars;
}

static size_t utf8_prefix_bytes(const char *value, size_t max_chars) {
  size_t bytes = 0;
  size_t chars = 0;
  while (value[bytes]) {
    if (((unsigned char)value[bytes] & 0xC0) != 0x80) {
      if (chars == max_chars)
        break;
      chars++;
    }
    bytes++;
  }
  return bytes;
}

int utf8_truncate(const char *value, char *out, size_t out_size,
                  size_t max_chars, const char *suffix) {
  if (!out || out_size == 0)
    return 0;
  out[0] = '\0';
  if (!value || !value[0] || max_chars == 0)
    return 1;

  size_t value_bytes = strlen(value);
  size_t value_chars =
      (size_t)count_visible_chars(value, (int)value_bytes);
  if (value_chars <= max_chars && value_bytes + 1 <= out_size) {
    memcpy(out, value, value_bytes + 1);
    return 1;
  }

  const char *tail = suffix ? suffix : "";
  size_t tail_bytes = strlen(tail);
  size_t tail_chars =
      (size_t)count_visible_chars(tail, (int)tail_bytes);
  if (tail_chars >= max_chars || tail_bytes + 1 > out_size) {
    tail = "";
    tail_bytes = 0;
    tail_chars = 0;
  }

  size_t keep_chars = max_chars - tail_chars;
  size_t bytes = utf8_prefix_bytes(value, keep_chars);
  while (bytes > 0 && bytes + tail_bytes + 1 > out_size)
    bytes = (size_t)get_prev_char_start(value, (int)bytes);
  memcpy(out, value, bytes);
  memcpy(out + bytes, tail, tail_bytes);
  out[bytes + tail_bytes] = '\0';
  return 0;
}
