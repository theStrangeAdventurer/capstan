#ifndef INPUT_H
#define INPUT_H

#define INPUT_BUFFER_SIZE 8192

void input_init(void);
const char *input_get_text(void);
int input_get_cursor(void);
void input_insert(int ch);
void input_set_text(const char *text);
void input_backspace(void);
void input_move_left(void);
void input_move_right(void);
void input_clear(void);

#endif
