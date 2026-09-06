#ifndef TEXT_LAYOUT_H
#define TEXT_LAYOUT_H
#include <stddef.h>
/* UTF-8 byte length and terminal cell width of the next character. */
size_t text_character(const char *text, size_t length, int *columns);
int text_columns(const char *text, size_t length);
/* A physical line, excluding newline. Always advances for nonempty text. */
size_t text_line_length(const char *text, int width);
/* Convert visual-mode codepoint columns to terminal cells and back. */
int text_char_to_cell(const char *text, size_t length, int characters);
int text_cell_to_char(const char *text, size_t length, int cells);
#endif
