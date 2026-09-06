#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <stddef.h>

#define MARKDOWN_BOLD 1
#define MARKDOWN_ITALIC 2
#define MARKDOWN_LITERAL 4
#define MARKDOWN_NO_SOURCE ((size_t)-1)

typedef struct {
  char *text;
  unsigned char *styles; /* One style per output byte; no terminal escapes. */
  size_t *source;        /* Offset into input, or MARKDOWN_NO_SOURCE. */
  size_t length, capacity;
} MarkdownView;

/* Optional adapter for literal tool output. Called once per source line. */
typedef int (*MarkdownLiteral)(size_t start, size_t end, void *context);
/* Transactional: on failure leaves view unchanged; caller can show raw text. */
int markdown_build(MarkdownView *view, const char *text, int width,
                   MarkdownLiteral literal, void *context);
void markdown_free(MarkdownView *view);

#endif
