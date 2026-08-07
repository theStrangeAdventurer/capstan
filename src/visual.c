#include "visual.h"
#include "linemap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_padding_line(int idx) {
    const LineInfo *li = linemap_get(idx);
    return li && li->role == LINE_PADDING;
}

static const char **g_texts = NULL;
static int g_texts_count = 0;
static int g_texts_owned = 0;

static struct {
    int active;
    int initialized;
    int selecting;
    int cursor_line;
    int cursor_col;
    int anchor_line;
    int anchor_col;
} g_visual;

int visual_cursor_visible(void) { return g_visual.active; }

int visual_is_active(void) { return g_visual.selecting; }

void visual_enter(void) {
    g_visual.active = 1;
    g_visual.selecting = 0;
    int line = linemap_count() - 1;
    while (line > 0 && is_padding_line(line))
        line--;
    if (line < 0)
        line = 0;
    g_visual.cursor_line = line;
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    g_visual.cursor_col = li ? li->char_count : 0;
    g_visual.initialized = 1;
}

void visual_resume(void) {
    if (!g_visual.initialized) {
        visual_enter();
        return;
    }
    g_visual.active = 1;
    g_visual.selecting = 0;
    int count = linemap_count();
    if (count <= 0) {
        g_visual.cursor_line = 0;
        g_visual.cursor_col = 0;
        return;
    }
    if (g_visual.cursor_line >= count)
        g_visual.cursor_line = count - 1;
    while (g_visual.cursor_line > 0 && is_padding_line(g_visual.cursor_line))
        g_visual.cursor_line--;
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    if (g_visual.cursor_col > (li ? li->char_count : 0))
        g_visual.cursor_col = li ? li->char_count : 0;
}

void visual_reset(void) {
    memset(&g_visual, 0, sizeof(g_visual));
    if (g_texts_owned)
        free(g_texts);
    g_texts = NULL;
    g_texts_count = 0;
    g_texts_owned = 0;
}

void visual_exit(void) {
    g_visual.active = 0;
    g_visual.selecting = 0;
}

void visual_enter_selection(void) {
    g_visual.selecting = 1;
    g_visual.anchor_line = g_visual.cursor_line;
    g_visual.anchor_col = g_visual.cursor_col;
}

void visual_exit_selection(void) { g_visual.selecting = 0; }

void visual_move_up(void) {
    int target = g_visual.cursor_line - 1;
    while (target >= 0 && is_padding_line(target))
        target--;
    if (target < 0) return;
    g_visual.cursor_line = target;
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    if (g_visual.cursor_col > (li ? li->char_count : 0))
        g_visual.cursor_col = li ? li->char_count : 0;
}

void visual_move_down(void) {
    int target = g_visual.cursor_line + 1;
    while (target < linemap_count() && is_padding_line(target))
        target++;
    if (target >= linemap_count()) return;
    g_visual.cursor_line = target;
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    if (g_visual.cursor_col > (li ? li->char_count : 0))
        g_visual.cursor_col = li ? li->char_count : 0;
}

void visual_move_left(void) {
    if (g_visual.cursor_col > 0)
        g_visual.cursor_col--;
}

void visual_move_right(void) {
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    int max_col = li ? li->char_count : 0;
    if (g_visual.cursor_col < max_col)
        g_visual.cursor_col++;
}

void visual_move_line_start(void) {
    g_visual.cursor_col = 0;
}

void visual_move_line_end(void) {
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    int max_col = li ? li->char_count : 0;
    g_visual.cursor_col = max_col > 0 ? max_col - 1 : 0;
}

void visual_set_texts(const char **texts, int count) {
    /* Takes ownership of the array, but not the message strings it points to. */
    if (g_texts_owned)
        free(g_texts);
    g_texts = texts;
    g_texts_count = count;
    g_texts_owned = 1;
}

static const char *get_current_line_text(int *out_len) {
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    if (!li || li->role == LINE_PADDING || !g_texts || li->msg_index >= (size_t)g_texts_count) {
        *out_len = 0;
        return NULL;
    }
    const char *text = g_texts[li->msg_index];
    if (!text) {
        *out_len = 0;
        return NULL;
    }
    *out_len = li->byte_end - li->byte_start;
    return text + li->byte_start;
}

static int char_at_col(const char *line, int line_len, int col) {
    int chars = 0;
    for (int i = 0; i < line_len; i++) {
        if ((line[i] & 0xC0) != 0x80) {
            if (chars == col)
                return (unsigned char)line[i];
            chars++;
        }
    }
    return -1;
}

static int line_char_count(const char *line, int line_len) {
    int chars = 0;
    for (int i = 0; i < line_len; i++) {
        if ((line[i] & 0xC0) != 0x80)
            chars++;
    }
    return chars;
}

static int is_word_char(int ch) {
    return ch > 0 && ch != ' ' && ch != '\t' && ch != '\n';
}

void visual_move_word_forward(void) {
    int line_len;
    const char *line = get_current_line_text(&line_len);
    if (!line) return;

    int total = line_char_count(line, line_len);
    int col = g_visual.cursor_col;

    if (col < total && is_word_char(char_at_col(line, line_len, col)))
        while (col < total && is_word_char(char_at_col(line, line_len, col)))
            col++;

    while (col < total && !is_word_char(char_at_col(line, line_len, col)))
        col++;

    if (col < total) {
        g_visual.cursor_col = col;
        return;
    }

    if (g_visual.cursor_line < linemap_count() - 1) {
        int target = g_visual.cursor_line + 1;
        while (target < linemap_count() && is_padding_line(target))
            target++;
        if (target >= linemap_count()) return;
        g_visual.cursor_line = target;
        const LineInfo *li = linemap_get(g_visual.cursor_line);
        g_visual.cursor_col = 0;
        int next_len;
        const char *next_line = get_current_line_text(&next_len);
        if (next_line) {
            int nc = 0;
            int next_total = line_char_count(next_line, next_len);
            while (nc < next_total && !is_word_char(char_at_col(next_line, next_len, nc)))
                nc++;
            g_visual.cursor_col = nc;
        }
        if (g_visual.cursor_col > (li ? li->char_count : 0))
            g_visual.cursor_col = li ? li->char_count : 0;
    }
}

void visual_move_word_backward(void) {
    int line_len;
    const char *line = get_current_line_text(&line_len);
    if (!line) return;

    int col = g_visual.cursor_col;
    int at_line_start = (col == 0);

    if (col > 0)
        col--;

    while (col > 0 && !is_word_char(char_at_col(line, line_len, col)))
        col--;

    if (col > 0 || (is_word_char(char_at_col(line, line_len, 0)) && !at_line_start)) {
        while (col > 0 && is_word_char(char_at_col(line, line_len, col - 1)))
            col--;
        g_visual.cursor_col = col;
        return;
    }

    if (g_visual.cursor_line > 0) {
        int target = g_visual.cursor_line - 1;
        while (target >= 0 && is_padding_line(target))
            target--;
        if (target < 0) return;
        g_visual.cursor_line = target;
        const LineInfo *li = linemap_get(g_visual.cursor_line);
        int new_len;
        const char *new_line = get_current_line_text(&new_len);
        if (new_line && new_len > 0) {
            int new_total = line_char_count(new_line, new_len);
            int nc = new_total - 1;
            while (nc > 0 && !is_word_char(char_at_col(new_line, new_len, nc)))
                nc--;
            while (nc > 0 && is_word_char(char_at_col(new_line, new_len, nc - 1)))
                nc--;
            g_visual.cursor_col = is_word_char(char_at_col(new_line, new_len, nc)) ? nc : 0;
        } else {
            g_visual.cursor_col = 0;
        }
        if (g_visual.cursor_col > (li ? li->char_count : 0))
            g_visual.cursor_col = li ? li->char_count : 0;
    }
}

void visual_get_cursor(int *line, int *col) {
    if (line) *line = g_visual.cursor_line;
    if (col) *col = g_visual.cursor_col;
}

void visual_set_cursor_line(int line) {
    if (line < 0) line = 0;
    if (line >= linemap_count()) line = linemap_count() - 1;

    if (is_padding_line(line)) {
        int fwd = line;
        while (fwd < linemap_count() && is_padding_line(fwd))
            fwd++;
        if (fwd < linemap_count())
            line = fwd;
        else
            while (line >= 0 && is_padding_line(line))
                line--;
    }

    g_visual.cursor_line = line;
    const LineInfo *li = linemap_get(g_visual.cursor_line);
    if (g_visual.cursor_col > (li ? li->char_count : 0))
        g_visual.cursor_col = li ? li->char_count : 0;
}

void visual_set_cursor(int line, int col) {
    int count = linemap_count();
    if (count <= 0) {
        g_visual.cursor_line = 0;
        g_visual.cursor_col = 0;
        return;
    }

    if (line < 0) line = 0;
    if (line >= count) line = count - 1;

    if (is_padding_line(line)) {
        int before = line;
        int after = line;
        while (before >= 0 && is_padding_line(before))
            before--;
        while (after < count && is_padding_line(after))
            after++;

        if (before < 0 && after < count)
            line = after;
        else if (after >= count && before >= 0)
            line = before;
        else if (before >= 0 && after < count) {
            int before_dist = line - before;
            int after_dist = after - line;
            line = before_dist <= after_dist ? before : after;
        }
    }

    if (line < 0 || line >= count) {
        g_visual.cursor_line = 0;
        g_visual.cursor_col = 0;
        return;
    }

    const LineInfo *li = linemap_get(line);
    int max_col = li ? li->char_count : 0;
    if (col < 0) col = 0;
    if (col > max_col) col = max_col;
    g_visual.cursor_line = line;
    g_visual.cursor_col = col;
}

void visual_enter_selection_at(int line, int col) {
    visual_set_cursor(line, col);
    visual_enter_selection();
}

void visual_selection_range(int *sl, int *sc, int *el, int *ec) {
    if (!g_visual.selecting) {
        *sl = *el = g_visual.cursor_line;
        *sc = *ec = g_visual.cursor_col;
        return;
    }
    if (g_visual.cursor_line < g_visual.anchor_line ||
        (g_visual.cursor_line == g_visual.anchor_line &&
         g_visual.cursor_col < g_visual.anchor_col)) {
        *sl = g_visual.cursor_line;
        *sc = g_visual.cursor_col;
        *el = g_visual.anchor_line;
        *ec = g_visual.anchor_col;
    } else {
        *sl = g_visual.anchor_line;
        *sc = g_visual.anchor_col;
        *el = g_visual.cursor_line;
        *ec = g_visual.cursor_col;
    }
}

static void copy_to_clipboard(const char *text) {
#ifdef __APPLE__
    FILE *p = popen("pbcopy", "w");
#else
    FILE *p = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (!p)
        p = popen("xsel --clipboard 2>/dev/null", "w");
#endif
    if (p) {
        fputs(text, p);
        pclose(p);
    }
}

static int byte_offset_for_col(const char *str, int col) {
    int chars = 0;
    int i = 0;
    while (str[i]) {
        if ((str[i] & 0xC0) != 0x80) {
            if (chars >= col)
                break;
            chars++;
        }
        i++;
    }
    return i;
}

void visual_yank(const char **msgs_texts, int msgs_count) {
    int sl, sc, el, ec;
    visual_selection_range(&sl, &sc, &el, &ec);

    char *buf = NULL;
    size_t buf_len = 0;
    size_t buf_cap = 0;

    for (int line = sl; line <= el; line++) {
        const LineInfo *li = linemap_get(line);
        if (!li || li->role == LINE_PADDING || li->msg_index >= (size_t)msgs_count)
            continue;

        const char *text = msgs_texts[li->msg_index];
        if (!text)
            continue;

        const char *line_text = text + li->byte_start;

        int start_col = (line == sl) ? sc : 0;
        int end_col = (line == el) ? ec : li->char_count;

        int byte_start = byte_offset_for_col(line_text, start_col);
        int byte_end = byte_offset_for_col(line_text, end_col);

        int seg_len = byte_end - byte_start;
        if (seg_len <= 0)
            seg_len = 0;

        if (buf_len + seg_len + 2 > buf_cap) {
            buf_cap = (buf_len + seg_len + 2) * 2;
            buf = realloc(buf, buf_cap);
        }

        if (seg_len > 0) {
            memcpy(buf + buf_len, line_text + byte_start, seg_len);
            buf_len += seg_len;
        }

        if (line < el) {
            buf[buf_len++] = '\n';
        }
    }

    if (buf && buf_len > 0) {
        buf[buf_len] = '\0';
        copy_to_clipboard(buf);
    }
    free(buf);
    g_visual.selecting = 0;
}
