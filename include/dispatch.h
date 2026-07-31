#ifndef DISPATCH_H
#define DISPATCH_H

#include <stddef.h>

#define MAX_COMMAND_LEN 64

int has_command(const char *input, char *command, size_t *cmd_end);
int dispatch_blocking_enter_allowed(int top_level_run_active);
void dispatch_submit(void);
void dispatch_tick(void);
int dispatch_queue_size(void);
int dispatch_queue_visible_size(void);
const char *dispatch_queue_at(int index);
void dispatch_queue_clear(void);
int dispatch_tab(void);
void dispatch_popup_result(void);

#endif
