#include "tui.h"
#include "agent.h"
#include "curses.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_tui(void) {
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_YELLOW, -1);
  }
  curs_set(1);
}

static int count_message_lines(const char *text, int width) {
  if (!text || !*text)
    return 1;
  int lines = 1;
  int col = 0;
  for (const char *p = text; *p; p++) {
    if (*p == '\n') {
      lines++;
      col = 0;
    } else if ((*p & 0xC0) != 0x80) {
      col++;
      if (col > width) {
        lines++;
        col = 1;
      }
    }
  }
  return lines;
}

static int spinner_tick = 0;
static int prev_loading = 0;
static const char *spinner_frames[] = {
  "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
#define SPINNER_FRAMES 10
#define SPINNER_COUNT  6
#define SPINNER_PHASE  2
static const int spinner_attrs[SPINNER_COUNT] = {
  A_BOLD,  // bright orange
  0,       // orange dimmed
  A_DIM,   // yellow dimmed
  0,       // yellow
  A_BOLD,  // orange
  0,       // orange dimmed
};

static int count_visible_chars_to(const char *str, int max_chars) {
  int bytes = 0, chars = 0;
  while (str[bytes]) {
    if ((str[bytes] & 0xC0) != 0x80) {
      if (chars >= max_chars)
        break;
      chars++;
    }
    bytes++;
  }
  return bytes;
}

void render_all(void) {
  int scroll_offset = g_scroll;
  const char *input = g_input_buf;
  int input_pos = g_cursor;
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int margin = MARGIN;
  int input_h = INPUT_WIN_HEIGHT;
  int msg_h = rows - input_h - 2 * margin;
  int inner_w = cols - 2 * margin;

  if (msg_h < 1 || inner_w < 1)
    return;

  WINDOW *msg_win = newwin(msg_h, inner_w, margin, margin);
  if (!msg_win)
    return;
  werase(msg_win);

  Messages *msgs = get_messages();

  int *line_counts = malloc(msgs->size * sizeof(int));
  int total_lines = 0;

  for (size_t i = 0; i < msgs->size; i++) {
    int l = count_message_lines(msgs->items[i]->text, inner_w);
    line_counts[i] = l;
    total_lines += l;
  }

  int max_scroll = total_lines > msg_h ? total_lines - msg_h : 0;
  if (scroll_offset > max_scroll)
    scroll_offset = max_scroll;
  if (scroll_offset < 0)
    scroll_offset = 0;

  int top_line = total_lines - msg_h - scroll_offset;
  if (top_line < 0)
    top_line = 0;

  int global_line = 0;
  int win_row = 0;

  for (size_t i = 0; i < msgs->size && win_row < msg_h; i++) {
    Message *msg = msgs->items[i];
    int cp = msg->role == MSG_USER ? 1 : 2;
    wattron(msg_win, COLOR_PAIR(cp));

    const char *p = msg->text;
    for (int l = 0; l < line_counts[i]; l++) {
      const char *line_end = p;
      int col = 0;
      while (*line_end && *line_end != '\n') {
        int is_char = (*line_end & 0xC0) != 0x80;
        if (is_char && col >= inner_w)
          break;
        line_end++;
        if (is_char)
          col++;
      }

      if (global_line >= top_line && win_row < msg_h) {
        mvwaddnstr(msg_win, win_row, 0, p, line_end - p);
        win_row++;
      }

      if (*line_end == '\n')
        line_end++;
      p = line_end;
      global_line++;
    }

    wattroff(msg_win, COLOR_PAIR(cp));
  }

  free(line_counts);

  int input_y = rows - input_h - margin;
  WINDOW *input_win = newwin(input_h, inner_w, input_y, margin);
  if (!input_win) {
    wnoutrefresh(msg_win);
    doupdate();
    delwin(msg_win);
    return;
  }

  werase(input_win);
  box(input_win, 0, 0);

  int content_w = inner_w - 2;
  int input_len = (int)strlen(input);
  int total_chars = count_visible_chars(input, input_len);
  int input_lines = total_chars ? (total_chars + content_w - 1) / content_w : 0;
  int skip_lines = input_lines > INPUT_CONTENT_LINES ? input_lines - INPUT_CONTENT_LINES : 0;
  int skip_chars = skip_lines * content_w;
  int skip_bytes = skip_chars ? count_visible_chars_to(input, skip_chars) : 0;

  const char *visible = input + skip_bytes;
  int rel_pos = input_pos - skip_bytes;
  if (rel_pos < 0) rel_pos = 0;

  int line1_bytes = 0;
  if (input_lines > skip_lines) {
    line1_bytes = count_visible_chars_to(visible, content_w);
    mvwaddnstr(input_win, 1, 1, visible, line1_bytes);
  }
  if (input_lines > skip_lines + 1)
    mvwaddstr(input_win, 2, 1, visible + line1_bytes);

  int cursor_line, cursor_col;
  if (line1_bytes == 0) {
    cursor_line = 1;
    cursor_col = 1;
  } else if (!(input_lines > skip_lines + 1) || rel_pos < line1_bytes) {
    cursor_line = 1;
    cursor_col = 1 + count_visible_chars(visible, rel_pos);
  } else {
    cursor_line = 2;
    cursor_col = 1 + count_visible_chars(visible + line1_bytes, rel_pos - line1_bytes);
  }
  wmove(input_win, cursor_line, cursor_col);

  int loading = http_is_loading();
  if (loading) {
    for (int s = 0; s < SPINNER_COUNT; s++) {
      int idx = ((spinner_tick / 4) + s * SPINNER_PHASE) % SPINNER_FRAMES;
      wattrset(stdscr, COLOR_PAIR(3) | spinner_attrs[s]);
      mvaddstr(rows - 1, MARGIN + 1 + s, spinner_frames[idx]);
    }
    wattrset(stdscr, A_NORMAL);
  } else if (prev_loading) {
    for (int s = 0; s < SPINNER_COUNT; s++)
      mvaddstr(rows - 1, MARGIN + 1 + s, "      ");
  }
  spinner_tick++;

  if (loading != prev_loading) {
    curs_set(loading ? 0 : 1);
    prev_loading = loading;
  }

  const char *prov = agent_provider_name();
  const char *model = agent_provider_model();
  if (prov && model) {
    char info[256];
    int n = snprintf(info, sizeof(info), "%s/%s", prov, model);
    attron(A_DIM);
    mvaddstr(rows - 1, cols - n - 2, info);
    attroff(A_DIM);
  }

  wnoutrefresh(stdscr);
  wnoutrefresh(msg_win);
  wnoutrefresh(input_win);
  doupdate();

  delwin(msg_win);
  delwin(input_win);
}

int count_visible_chars(const char *str, int byte_pos) {
  int chars = 0;
  for (int i = 0; i < byte_pos && str[i]; i++) {
    // Это классический способ работать с UTF8 символами,
    // по сути эта проверка позволяет игногировать все хвосты многобайтовых
    // символов и считать только их головы
    if ((str[i] & 0xC0) != 0x80)
      chars++;
  }
  return chars;
}

int get_prev_char_start(const char *str, int pos) {
  if (pos <= 0)
    return pos;
  pos--;
  while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
    pos--;
  }
  return pos;
}
