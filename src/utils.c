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

int replace_with(char *input, size_t input_size, char *from, char *to) {
  if (input == NULL || from == NULL || to == NULL || input_size == 0) {
    return -1;
  }

  char temp[input_size];
  strcpy(temp, input);
  char *p = strstr(temp, from);

  if (!p) {
    return 0;
  }

  size_t prefix_len = p - temp;
  char suffix[input_size];
  strcpy(suffix, p + strlen(from));
  memcpy(input, temp, prefix_len);
  input[prefix_len] = '\0';
  strcat(input, to);
  strcat(input, suffix);

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
