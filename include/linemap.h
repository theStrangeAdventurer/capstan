#ifndef LINEMAP_H
#define LINEMAP_H

#include <stddef.h>

typedef struct {
    size_t msg_index;
    int byte_start;
    int byte_end;
    int char_count;
    int role;
} LineInfo;

void linemap_build(void **msgs_data, int *msgs_roles, int msgs_count,
                   const char **msgs_texts, int width);
const LineInfo *linemap_get(int idx);
int linemap_count(void);
void linemap_free(void);

#endif
