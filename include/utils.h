#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

char *my_strdup(const char *s);
int replace_with(char *input, size_t input_size, const char *from,
                 const char *to);
int get_prev_char_start(const char *str, int pos);
int count_visible_chars(const char *str, int byte_pos);
int utf8_truncate(const char *value, char *out, size_t out_size,
                  size_t max_chars, const char *suffix);

#endif
