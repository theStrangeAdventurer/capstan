#include "linemap.h"
#include <stdlib.h>
#include <string.h>

static LineInfo *g_lines = NULL;
static int g_count = 0;
static int g_capacity = 0;

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

        if (!text || !*text) {
            linemap_append(i, 0, 0, 0, role);
            continue;
        }

        const char *p = text;
        while (*p) {
            const char *line_start = p;
            int col = 0;
            const char *line_end = p;

            while (*line_end && *line_end != '\n') {
                int is_char = (*line_end & 0xC0) != 0x80;
                if (is_char && col >= width)
                    break;
                line_end++;
                if (is_char)
                    col++;
            }

            int byte_start = (int)(line_start - text);
            int byte_end = (int)(line_end - text);
            int char_count = count_line_chars(line_start, line_end);

            linemap_append(i, byte_start, byte_end, char_count, role);

            if (*line_end == '\n')
                line_end++;
            p = line_end;
        }
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
