#ifndef TUI_H
#define TUI_H

void redraw(int x, int y, char *input);

void redraw_backspace(int x, int y, int pos);

int get_prev_char_start(const char *str, int pos);

int count_visible_chars(const char *str, int byte_pos);

void redraw_char(int x, int y, char *input, int pos);
#endif
