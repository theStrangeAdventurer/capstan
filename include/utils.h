#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
void redraw(int x, int y, char *input);

int replace_with(char *input, size_t input_size, char *from, char *to);

#endif
