#ifndef LINKS_H
#define LINKS_H

#include <stddef.h>

#define LINK_URL 1
#define LINK_PATH 2

/* Openable span inside projected message text (byte offsets, half-open). */
typedef struct {
  size_t start, end;
  int kind;
} LinkSpan;

/* Finds web URLs and path-looking tokens. Text is never modified. */
int links_scan(const char *text, size_t length, LinkSpan *spans, int max);
/* 1 when the span is actionable: paths are skipped inside code, while web links
 * stay clickable there too. */
int links_allowed(const LinkSpan *span, int literal);
/* Index of the span containing offset, or -1. */
int links_find(const LinkSpan *spans, int count, size_t offset);
/* Opens a scanned span: web links in the browser, paths in the file manager.
 * Relative paths resolve against base_dir. Returns 1 on success, otherwise
 * writes a short reason for the caller to show. */
int links_open(const char *text, size_t length, int kind, const char *base_dir,
               char *error, size_t error_size);
/* Opens already-extracted text: the first URL or path inside it. */
int links_open_text(const char *text, const char *base_dir, char *error,
                    size_t error_size);

#endif
