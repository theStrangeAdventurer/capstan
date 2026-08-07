#ifndef DIFF_HIGHLIGHT_H
#define DIFF_HIGHLIGHT_H

typedef enum {
  DIFF_HIGHLIGHT_NONE = 0,
  DIFF_HIGHLIGHT_ADD,
  DIFF_HIGHLIGHT_DELETE,
} DiffHighlightKind;

DiffHighlightKind diff_highlight_kind(const char *logical_line_start);

#endif
