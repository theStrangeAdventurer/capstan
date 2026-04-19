#include <stddef.h>
#include <string.h>

/**
 * replace string to another string and put it in Buffer
 * returns 0 - nothing was replaced | 1 - ok | -1 - invalid call
 */
int replace_with(char *input, size_t input_size, char *from, char *to) {
  if (input == NULL || from == NULL || to == NULL || input_size == 0) {
    return -1;
  }

  char temp[input_size];
  strcpy(temp, input);          // копируем весь input
  char *p = strstr(temp, from); // ищем в копии, не в input

  if (!p) {
    return 0;
  }

  size_t prefix_len = p - temp;
  char suffix[input_size];
  strcpy(suffix, p + strlen(from)); // суффикс после "/hi"
  memcpy(input, temp, prefix_len);  // префикс
  input[prefix_len] = '\0';
  strcat(input, to); // emoji
  strcat(input, suffix);

  return 1;
}
