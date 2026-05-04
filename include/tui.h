#ifndef TUI_H
#define TUI_H

#define INPUT_WIN_HEIGHT 3
#define MARGIN 1

void init_tui(void);
void render_all(int scroll_offset, const char *input, int input_pos);
int get_prev_char_start(const char *str, int pos);
int count_visible_chars(const char *str, int byte_pos);

#endif
