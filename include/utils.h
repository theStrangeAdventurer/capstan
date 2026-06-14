#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

char *my_strdup(const char *s);
int replace_with(char *input, size_t input_size, char *from, char *to);
int get_prev_char_start(const char *str, int pos);
int count_visible_chars(const char *str, int byte_pos);

#endif
