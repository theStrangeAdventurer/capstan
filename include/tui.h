#ifndef TUI_H
#define TUI_H

#define INPUT_WIN_HEIGHT 3
#define MARGIN 1

extern int g_scroll;
extern char g_input_buf[];
extern int g_cursor;

void init_tui(void);
void render_all(void);
int get_prev_char_start(const char *str, int pos);
int count_visible_chars(const char *str, int byte_pos);

#endif
