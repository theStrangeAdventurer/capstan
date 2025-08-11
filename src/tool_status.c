#include "tool_status.h"
#include <string.h>

static int line_starts_with(const char *line, int len, const char *prefix) {
  int prefix_len = (int)strlen(prefix);
  return line && len >= prefix_len && strncmp(line, prefix, prefix_len) == 0;
}

static int line_contains_status(const char *line, int len,
                                const char *status) {
  int status_len = (int)strlen(status);
  if (!line || len < status_len)
    return 0;
  for (int i = 0; i <= len - status_len; i++) {
    int end = i + status_len;
    if ((i == 0 || line[i - 1] == ' ') &&
        memcmp(line + i, status, (size_t)status_len) == 0 &&
        (end == len || line[end] == ' ' || line[end] == ':'))
      return 1;
  }
  return 0;
}

int tool_status_starts_line(const char *line, int len) {
  return line_starts_with(line, len, "⚙") ||
         line_starts_with(line, len, "  $ ");
}

int tool_status_ends_line(const char *line, int len) {
  return line_contains_status(line, len, "— done") ||
         line_contains_status(line, len, "— error") ||
         line_contains_status(line, len, "— denied") ||
         line_contains_status(line, len, "— skipped");
}
