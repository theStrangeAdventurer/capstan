#include "dispatch.h"
#include <string.h>

int has_command(const char *input, char *command, size_t *cmd_end) {
  const char *start = input;
  while (*start == ' ')
    start++;
  if (*start != '/')
    return 0;

  const char *end = start;
  while (*end && *end != ' ' && *end != '\0')
    end++;

  size_t len = end - start;
  if (len >= MAX_COMMAND_LEN)
    len = MAX_COMMAND_LEN - 1;
  strncpy(command, start, len);
  command[len] = '\0';
  *cmd_end = end - input;
  return 1;
}
