#include "linemap.h"
#include "text_layout.h"
#include <stdlib.h>
#include <string.h>

/* Blank rows between the path text and the picture. */
#define LINEMAP_IMAGE_SPACERS 2

static LineInfo *g_lines = NULL;
static int g_count = 0;
static int g_capacity = 0;
static LinemapImageRows g_image_rows = NULL;

void linemap_set_image_rows(LinemapImageRows rows) { g_image_rows = rows; }

static void linemap_append(size_t msg_index, int byte_start, int byte_end,
                           int char_count, int role) {
    if (g_count >= g_capacity) {
        g_capacity = g_capacity ? g_capacity * 2 : 64;
        g_lines = realloc(g_lines, g_capacity * sizeof(LineInfo));
    }
    LineInfo *li = &g_lines[g_count++];
    li->msg_index = msg_index;
    li->byte_start = byte_start;
    li->byte_end = byte_end;
    li->char_count = char_count;
    li->role = role;
}

static int count_line_chars(const char *start, const char *end) {
    int chars = 0;
    for (const char *p = start; p < end; p++) {
        if ((*p & 0xC0) != 0x80)
            chars++;
    }
    return chars;
}

void linemap_build(void **msgs_data, int *msgs_roles, int msgs_count,
                   const char **msgs_texts, int width) {
    (void)msgs_data;
    linemap_free();

    for (int i = 0; i < msgs_count; i++) {
        const char *text = msgs_texts[i];
        int role = msgs_roles[i];

        linemap_append(i, 0, 0, 0, LINE_PADDING);

        if (!text || !*text) {
            linemap_append(i, 0, 0, 0, role);
        } else {
            const char *p = text;
            while (1) {
                const char *line_start = p;
                const char *line_end = p + text_line_length(p, width);

                int byte_start = (int)(line_start - text);
                int byte_end = (int)(line_end - text);
                int char_count = count_line_chars(line_start, line_end);

                linemap_append(i, byte_start, byte_end, char_count, role);

                /* Reserve rows below a whole-line image path, so the image band
                 * is part of the same line map that drives drawing and scrolling. */
                if (g_image_rows && byte_end > byte_start &&
                    (*line_end == '\n' || *line_end == '\0')) {
                    int rows = g_image_rows(line_start, (size_t)(line_end - line_start));
                    /* Layout per image: spacer rows, the picture band, then one row
                     * for the caption under the picture. */
                    if (rows > 0) {
                        for (int row = 0; row < LINEMAP_IMAGE_SPACERS; row++)
                            linemap_append(i, byte_start, byte_end, 0, LINE_IMAGE);
                        for (int row = 0; row < rows; row++)
                            linemap_append(i, byte_start, byte_end,
                                           row == 0 ? rows : 0, LINE_IMAGE);
                        linemap_append(i, byte_start, byte_end, LINE_IMAGE_CAPTION,
                                       LINE_IMAGE);
                    }
                }

                if (*line_end == '\n') {
                    p = line_end + 1;
                    continue;
                }
                if (*line_end == '\0')
                    break;
                p = line_end;
            }
        }

        linemap_append(i, 0, 0, 0, LINE_PADDING);
    }
}

const LineInfo *linemap_get(int idx) {
    if (idx < 0 || idx >= g_count)
        return NULL;
    return &g_lines[idx];
}

int linemap_count(void) { return g_count; }

void linemap_free(void) {
    free(g_lines);
    g_lines = NULL;
    g_count = 0;
    g_capacity = 0;
}
