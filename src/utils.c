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
