#ifndef DISPATCH_H
#define DISPATCH_H

#include <stddef.h>

#define MAX_COMMAND_LEN 64

int has_command(const char *input, char *command, size_t *cmd_end);
void dispatch_submit(void);
void dispatch_popup_result(void);

#endif
