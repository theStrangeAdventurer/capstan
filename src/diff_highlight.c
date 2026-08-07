#include "diff_highlight.h"
#include <string.h>

DiffHighlightKind diff_highlight_kind(const char *logical_line_start) {
  if (!logical_line_start || !logical_line_start[0])
    return DIFF_HIGHLIGHT_NONE;
  if (logical_line_start[0] == '+' &&
      strncmp(logical_line_start, "+++", 3) != 0)
    return DIFF_HIGHLIGHT_ADD;
  if (logical_line_start[0] == '-' &&
      strncmp(logical_line_start, "---", 3) != 0)
    return DIFF_HIGHLIGHT_DELETE;
  return DIFF_HIGHLIGHT_NONE;
}
