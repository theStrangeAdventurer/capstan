#ifndef VISUAL_H
#define VISUAL_H

int visual_cursor_visible(void);
int visual_is_active(void);
void visual_enter(void);
void visual_resume(void);
void visual_reset(void);
void visual_exit(void);
void visual_enter_selection(void);
void visual_exit_selection(void);
void visual_move_up(void);
void visual_move_down(void);
void visual_move_left(void);
void visual_move_right(void);
void visual_move_line_start(void);
void visual_move_line_end(void);
void visual_move_word_forward(void);
void visual_move_word_backward(void);
void visual_set_texts(const char **texts, int count);
void visual_yank(const char **msgs_texts, int msgs_count);
void visual_get_cursor(int *line, int *col);
void visual_set_cursor_line(int line);
void visual_set_cursor(int line, int col);
void visual_enter_selection_at(int line, int col);
void visual_selection_range(int *sl, int *sc, int *el, int *ec);

#endif
