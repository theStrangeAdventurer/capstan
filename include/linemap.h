#ifndef LINEMAP_H
#define LINEMAP_H

#include <stddef.h>

#define LINE_PADDING  -1
#define LINE_IMAGE    -2 /* Reserved screen rows for a rendered image. */
/* char_count of an image row that carries the caption below the picture. */
#define LINE_IMAGE_CAPTION -1

typedef struct {
    size_t msg_index;
    int byte_start;
    int byte_end;
    int char_count;
    int role;
} LineInfo;

/* Optional per-line reservation: image height in rows for a whole logical line,
 * or 0 to keep the line as text. */
typedef int (*LinemapImageRows)(const char *line, size_t length);

void linemap_set_image_rows(LinemapImageRows rows);
void linemap_build(void **msgs_data, int *msgs_roles, int msgs_count,
                   const char **msgs_texts, int width);
const LineInfo *linemap_get(int idx);
int linemap_count(void);
void linemap_free(void);

#endif
