#define _XOPEN_SOURCE 700
#include "text_layout.h"
#include <stdint.h>
#include <wchar.h>

size_t text_character(const char *text, size_t length, int *columns) {
  if (!length) { *columns = 0; return 0; }
  unsigned char first = (unsigned char)text[0];
  size_t n = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
             first >= 0xe0 && first <= 0xef ? 3 :
             first >= 0xf0 && first <= 0xf4 ? 4 : 1;
  uint32_t cp = first & (n == 1 ? 0xff : n == 2 ? 0x1f : n == 3 ? 0x0f : 7);
  if (n > length) n = 1;
  for (size_t i = 1; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if ((c & 0xc0) != 0x80) { n = 1; cp = first; break; }
    cp = (cp << 6) | (c & 0x3f);
  }
  int w = wcwidth((wchar_t)cp);
  *columns = w < 0 ? 1 : w;
  return n;
}

int text_columns(const char *text, size_t length) {
  int total = 0;
  for (size_t i = 0; i < length;) {
    int w;
    i += text_character(text + i, length - i, &w);
    total += w;
  }
  return total;
}

size_t text_line_length(const char *text, int width) {
  size_t i = 0;
  int cells = 0;
  if (width < 1) width = 1;
  while (text[i] && text[i] != '\n') {
    /* Decode at most one UTF-8 character, never scan the unwrapped suffix.
     * Include trailing zero-width marks even when the row is already full. */
    size_t available = 1;
    while (available < 4 && text[i + available] && text[i + available] != '\n')
      available++;
    int w;
    size_t n = text_character(text + i, available, &w);
    if (w && cells + w > width && i) break;
    i += n;
    cells += w;
  }
  return i;
}

int text_char_to_cell(const char *text, size_t length, int characters) {
  int cells = 0;
  for (size_t i = 0; i < length && characters-- > 0;) {
    int w;
    i += text_character(text + i, length - i, &w);
    cells += w;
  }
  return cells;
}

int text_cell_to_char(const char *text, size_t length, int cells) {
  int chars = 0, used = 0;
  for (size_t i = 0; i < length;) {
    int w;
    size_t n = text_character(text + i, length - i, &w);
    if (w && used + w > cells) break;
    used += w;
    i += n;
    chars++;
  }
  return chars;
}
