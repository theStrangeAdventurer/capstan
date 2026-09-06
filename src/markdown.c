#include "markdown.h"
#include "text_layout.h"
#include "../vendor/md4c/md4c.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void markdown_free(MarkdownView *view) {
  free(view->text);
  free(view->styles);
  free(view->source);
  *view = (MarkdownView){0};
}

static int append(MarkdownView *v, const char *s, size_t n, int style,
                  size_t source) {
  if (n > SIZE_MAX - v->length - 1) return -1;
  size_t needed = v->length + n + 1;
  if (needed > v->capacity) {
    size_t cap = v->capacity ? v->capacity : 128;
    while (cap < needed) {
      if (cap > SIZE_MAX / 2) return -1;
      cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(size_t)) return -1;
    char *text = realloc(v->text, cap);
    if (!text) return -1;
    v->text = text;
    unsigned char *styles = realloc(v->styles, cap);
    if (!styles) return -1;
    v->styles = styles;
    size_t *offsets = realloc(v->source, cap * sizeof(size_t));
    if (!offsets) return -1;
    v->source = offsets;
    v->capacity = cap;
  }
  memcpy(v->text + v->length, s, n);
  memset(v->styles + v->length, style, n);
  for (size_t i = 0; i < n; i++)
    v->source[v->length + i] = source == MARKDOWN_NO_SOURCE ? source : source + i;
  v->length += n;
  v->text[v->length] = '\0';
  return 0;
}

static int generated(MarkdownView *v, const char *s, int style) {
  return append(v, s, strlen(s), style, MARKDOWN_NO_SOURCE);
}

static int copy_part(MarkdownView *to, const MarkdownView *from,
                     size_t start, size_t end) {
  size_t offset = to->length;
  if (append(to, from->text + start, end - start, 0, MARKDOWN_NO_SOURCE)) return -1;
  memcpy(to->styles + offset, from->styles + start, end - start);
  memcpy(to->source + offset, from->source + start, (end - start) * sizeof(size_t));
  return 0;
}

static int spaces(MarkdownView *v, int count) {
  while (count-- > 0) if (generated(v, " ", 0)) return -1;
  return 0;
}

typedef struct { MarkdownView view; MD_ALIGN align; } Cell;
typedef struct { unsigned next; int ordered; } List;
typedef struct {
  MarkdownView *out;
  const char *source;
  size_t source_length;
  int width, bold, italic, code;
  unsigned list_depth;
  List lists[128];
  Cell *cells;
  size_t cell_count, cell_capacity;
  unsigned columns;
  int in_cell, header;
} Renderer;

static MarkdownView *destination(Renderer *r) {
  return r->in_cell ? &r->cells[r->cell_count - 1].view : r->out;
}

static int newline(MarkdownView *v) {
  return v->length && v->text[v->length - 1] != '\n' ? generated(v, "\n", 0) : 0;
}

static int border(Renderer *r, const int *widths, const char *left,
                   const char *join, const char *right) {
  if (generated(r->out, left, 0)) return -1;
  for (unsigned c = 0; c < r->columns; c++) {
    for (int j = 0; j < widths[c] + 2; j++)
      if (generated(r->out, "─", 0)) return -1;
    if (generated(r->out, c + 1 == r->columns ? right : join, 0)) return -1;
  }
  return generated(r->out, "\n", 0);
}

static size_t cell_end(const MarkdownView *v, size_t start, int width) {
  size_t n = text_line_length(v->text + start, width);
  /* Prefer word boundaries, but split long tokens without dropping bytes. */
  if (start + n < v->length && v->text[start + n] != ' ') {
    for (size_t i = n; i > 0; i--)
      if (v->text[start + i - 1] == ' ') return start + i;
  }
  return start + n;
}

static int render_table(Renderer *r) {
  if (!r->columns || !r->cell_count) return 0;
  int *widths = calloc(r->columns, sizeof(int));
  size_t *positions = calloc(r->columns, sizeof(size_t));
  if (!widths || !positions) { free(widths); free(positions); return -1; }
  int result = -1;
  for (unsigned c = 0; c < r->columns; c++) widths[c] = 2;
  for (size_t i = 0; i < r->cell_count; i++) {
    int w = text_columns(r->cells[i].view.text, r->cells[i].view.length);
    if (w > widths[i % r->columns]) widths[i % r->columns] = w;
  }
  /* Very narrow terminals use labelled fields, never truncate a column. */
  if ((size_t)r->width < (size_t)r->columns * 5 + 1) {
    for (size_t row = r->columns; row < r->cell_count; row += r->columns) {
      for (unsigned c = 0; c < r->columns && row + c < r->cell_count; c++) {
        MarkdownView *head = &r->cells[c].view, *value = &r->cells[row + c].view;
        if (copy_part(r->out, head, 0, head->length) || generated(r->out, ": ", 0) ||
            copy_part(r->out, value, 0, value->length) || generated(r->out, "\n", 0)) goto done;
      }
      if (row + r->columns < r->cell_count && generated(r->out, "\n", 0)) goto done;
    }
    if (r->cell_count == r->columns) {
      for (unsigned c = 0; c < r->columns; c++) {
        MarkdownView *head = &r->cells[c].view;
        if (copy_part(r->out, head, 0, head->length) || generated(r->out, "\n", 0)) goto done;
      }
    }
    result = 0;
    goto done;
  }
  long long total = 1 + 3LL * r->columns;
  for (unsigned c = 0; c < r->columns; c++) total += widths[c];
  while (total > r->width) {
    unsigned largest = 0;
    for (unsigned c = 1; c < r->columns; c++)
      if (widths[c] > widths[largest]) largest = c;
    /* Cap in one step when possible, avoiding work proportional to long cells. */
    int next = 2;
    for (unsigned c = 0; c < r->columns; c++)
      if (c != largest && widths[c] > next) next = widths[c];
    if (next >= widths[largest]) next = widths[largest] - 1;
    long long shrink = widths[largest] - next;
    if (shrink > total - r->width) shrink = total - r->width;
    widths[largest] -= (int)shrink;
    total -= shrink;
  }
  if (border(r, widths, "┌", "┬", "┐")) goto done;
  for (size_t row = 0; row < r->cell_count; row += r->columns) {
    memset(positions, 0, r->columns * sizeof(size_t));
    int more;
    do {
      more = 0;
      if (generated(r->out, "│", 0)) goto done;
      for (unsigned c = 0; c < r->columns; c++) {
        if (row + c >= r->cell_count) goto done;
        Cell *cell = &r->cells[row + c];
        size_t start = positions[c], end = cell_end(&cell->view, start, widths[c]);
        int used = text_columns(cell->view.text + start, end - start);
        int pad = widths[c] - used;
        int left = cell->align == MD_ALIGN_RIGHT ? pad : cell->align == MD_ALIGN_CENTER ? pad / 2 : 0;
        if (spaces(r->out, 1 + left) || copy_part(r->out, &cell->view, start, end) ||
            spaces(r->out, 1 + pad - left) || generated(r->out, "│", 0)) goto done;
        positions[c] = end;
        if (end < cell->view.length) more = 1;
      }
      if (generated(r->out, "\n", 0)) goto done;
    } while (more);
    if (row + r->columns < r->cell_count &&
        border(r, widths, "├", "┼", "┤")) goto done;
  }
  if (border(r, widths, "└", "┴", "┘")) goto done;
  result = 0;
done:
  free(widths);
  free(positions);
  return result;
}

static void clear_cells(Renderer *r) {
  for (size_t i = 0; i < r->cell_count; i++) markdown_free(&r->cells[i].view);
  free(r->cells);
  r->cells = NULL;
  r->cell_count = r->cell_capacity = 0;
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *data) {
  Renderer *r = data;
  switch (type) {
  case MD_BLOCK_H: r->bold++; break;
  case MD_BLOCK_CODE: r->code++; break;
  case MD_BLOCK_UL: case MD_BLOCK_OL:
    if (r->list_depth == 128) return -1;
    r->lists[r->list_depth++] = (List){
      .ordered = type == MD_BLOCK_OL,
      .next = type == MD_BLOCK_OL ? ((MD_BLOCK_OL_DETAIL *)detail)->start : 0
    };
    break;
  case MD_BLOCK_LI: {
    if (newline(r->out) || spaces(r->out, r->list_depth > 0 ? 2 * (int)(r->list_depth - 1) : 0)) return -1;
    List *list = r->list_depth ? &r->lists[r->list_depth - 1] : NULL;
    char marker[32];
    if (list && list->ordered) snprintf(marker, sizeof(marker), "%u. ", list->next++);
    else snprintf(marker, sizeof(marker), "- ");
    return generated(r->out, marker, 0);
  }
  case MD_BLOCK_QUOTE: return generated(r->out, "> ", 0);
  case MD_BLOCK_HR: return generated(r->out, "───\n", 0);
  case MD_BLOCK_TABLE:
    r->columns = ((MD_BLOCK_TABLE_DETAIL *)detail)->col_count;
    return newline(r->out);
  case MD_BLOCK_TH: case MD_BLOCK_TD: {
    if (r->cell_count == r->cell_capacity) {
      size_t cap = r->cell_capacity ? r->cell_capacity * 2 : 16;
      if (cap > SIZE_MAX / sizeof(Cell)) return -1;
      Cell *cells = realloc(r->cells, cap * sizeof(Cell));
      if (!cells) return -1;
      r->cells = cells;
      r->cell_capacity = cap;
    }
    Cell *cell = &r->cells[r->cell_count++];
    *cell = (Cell){.align = ((MD_BLOCK_TD_DETAIL *)detail)->align};
    r->in_cell = 1;
    r->header = type == MD_BLOCK_TH;
    return generated(&cell->view, "", 0);
  }
  default: break;
  }
  return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *data) {
  (void)detail;
  Renderer *r = data;
  switch (type) {
  case MD_BLOCK_H: r->bold--; return generated(r->out, "\n\n", 0);
  case MD_BLOCK_CODE: r->code--; return newline(r->out);
  case MD_BLOCK_P: return generated(r->out, r->list_depth ? "\n" : "\n\n", 0);
  case MD_BLOCK_LI: return newline(r->out);
  case MD_BLOCK_UL: case MD_BLOCK_OL:
    if (r->list_depth) r->list_depth--;
    return newline(r->out);
  case MD_BLOCK_TH: case MD_BLOCK_TD: r->in_cell = r->header = 0; break;
  case MD_BLOCK_TABLE: {
    int result = render_table(r);
    clear_cells(r);
    return result;
  }
  default: break;
  }
  return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *data) {
  (void)detail;
  Renderer *r = data;
  if (type == MD_SPAN_EM) r->italic++;
  if (type == MD_SPAN_STRONG) r->bold++;
  if (type == MD_SPAN_CODE) {
    r->code++;
    return generated(destination(r), "`", MARKDOWN_LITERAL);
  }
  return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *data) {
  Renderer *r = data;
  if (type == MD_SPAN_EM) r->italic--;
  if (type == MD_SPAN_STRONG) r->bold--;
  if (type == MD_SPAN_CODE) {
    r->code--;
    return generated(destination(r), "`", MARKDOWN_LITERAL);
  }
  if (type == MD_SPAN_A) {
    MD_SPAN_A_DETAIL *link = detail;
    if (!link->is_autolink) {
      if (generated(destination(r), " (", 0) ||
          append(destination(r), link->href.text, link->href.size, 0, MARKDOWN_NO_SOURCE) ||
          generated(destination(r), ")", 0)) return -1;
    }
  }
  return 0;
}

static int text_callback(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *data) {
  Renderer *r = data;
  int style = (r->bold || r->header ? MARKDOWN_BOLD : 0) |
              (r->italic ? MARKDOWN_ITALIC : 0) | (r->code ? MARKDOWN_LITERAL : 0);
  if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR)
    return generated(destination(r), r->in_cell ? " " : "\n", style);
  uintptr_t address = (uintptr_t)text, base = (uintptr_t)r->source;
  size_t source = address >= base && address - base <= r->source_length &&
                  size <= r->source_length - (address - base) ? address - base : MARKDOWN_NO_SOURCE;
  return append(destination(r), text, size, style, source);
}

static int parse(Renderer *r, size_t start, size_t end) {
  if (end == start) return 0;
  if (end - start > UINT_MAX) return -1;
  MD_PARSER parser = {.flags = MD_FLAG_TABLES | MD_FLAG_NOHTML,
    .enter_block = enter_block, .leave_block = leave_block,
    .enter_span = enter_span, .leave_span = leave_span, .text = text_callback};
  size_t before = r->out->length;
  if (md_parse(r->source + start, (MD_SIZE)(end - start), &parser, r)) return -1;
  /* Keep boundary newlines exact: adjacent literal output must not move. */
  while (r->out->length > before && r->out->text[r->out->length - 1] == '\n')
    r->out->length--;
  r->out->text[r->out->length] = '\0';
  size_t trailing = end;
  while (trailing > start && r->source[trailing - 1] == '\n') trailing--;
  return append(r->out, r->source + trailing, end - trailing, 0, trailing);
}

/* Preserve fences verbatim (including language and diff markers), with arbitrary
 * delimiter length, tildes, and up to three leading spaces. */
static size_t fence(const char *s, size_t n, char *mark, size_t *tail) {
  size_t i = 0;
  while (i < n && i < 3 && s[i] == ' ') i++;
  if (i == n || (s[i] != '`' && s[i] != '~')) return 0;
  *mark = s[i];
  size_t start = i;
  while (i < n && s[i] == *mark) i++;
  *tail = i;
  return i - start >= 3 ? i - start : 0;
}

int markdown_build(MarkdownView *view, const char *text, int width,
                   MarkdownLiteral literal, void *context) {
  if (!text) text = "";
  MarkdownView out = {0};
  Renderer r = {.out = &out, .source = text, .source_length = strlen(text),
                .width = width > 0 ? width : 1};
  if (generated(&out, "", 0)) goto fail;
  size_t chunk = 0, fence_length = 0;
  char fence_mark = 0;
  for (size_t start = 0; start < r.source_length;) {
    size_t end = start + strcspn(text + start, "\n");
    size_t next = end + (text[end] == '\n');
    int raw = literal && literal(start, end, context);
    char mark = 0;
    size_t tail = 0, run = fence(text + start, end - start, &mark, &tail);
    if (fence_length) {
      /* Provenance may mark a closing diff fence literal too. It still closes
       * our active fence; only opening fences are suppressed in tool output. */
      raw = 1;
      while (start + tail < end && (text[start + tail] == ' ' || text[start + tail] == '\t')) tail++;
      if (run >= fence_length && mark == fence_mark && start + tail == end) fence_length = 0;
    } else if (!raw && run &&
               (mark != '`' || !memchr(text + start + tail, '`', end - start - tail))) {
      /* CommonMark forbids backticks in a backtick fence's info string.
       * Leave such lines to MD4C (they may be ordinary inline code). */
      raw = 1;
      fence_length = run;
      fence_mark = mark;
    }
    if (raw) {
      if (parse(&r, chunk, start) || append(&out, text + start, next - start, MARKDOWN_LITERAL, start)) goto fail;
      chunk = next;
    }
    start = next;
  }
  if (parse(&r, chunk, r.source_length)) goto fail;
  markdown_free(view);
  *view = out;
  return 1;
fail:
  clear_cells(&r);
  markdown_free(&out);
  return 0;
}
