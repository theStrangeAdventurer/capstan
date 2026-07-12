#ifndef INPUT_HISTORY_H
#define INPUT_HISTORY_H

#include <stddef.h>

#define INPUT_HISTORY_LIMIT 20

void input_history_reset(void);
int input_history_load(const char *workdir);
int input_history_add(const char *text);
const char *input_history_prev(const char *current_input);
const char *input_history_next(const char *current_input);
const char *input_history_path(void);
int input_history_count(void);
const char *input_history_entry(int index);

#endif
