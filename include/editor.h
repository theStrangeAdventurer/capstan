#ifndef EDITOR_H
#define EDITOR_H

#include <stddef.h>

/* Reads at most capacity - 1 bytes. On failure destination is unchanged and
 * error describes the cause. Never removes or modifies the source file. */
int editor_read_prompt_file(const char *path, char *destination, size_t capacity,
                            char *error, size_t error_capacity);

int editor_open_prompt(const char *initial_text);

#endif
