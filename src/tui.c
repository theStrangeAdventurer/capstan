#include "tui.h"
#include "agent.h"
#include "curses.h"
#include "http.h"
#include <stdlib.h>
#include <string.h>

void init_tui(void) {
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_GREEN, -1);
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
  mvwaddstr(input_win, 1, 1, input);
  int vis_pos = count_visible_chars(input, input_pos);
  wmove(input_win, 1, 1 + vis_pos);

  int loading = http_is_loading();
  if (loading) {
    int idx = (spinner_tick / 4) % SPINNER_FRAMES;
    attron(A_BOLD);
    mvaddstr(rows - 1, MARGIN + 1, spinner_frames[idx]);
    attroff(A_BOLD);
  } else if (prev_loading) {
    mvaddstr(rows - 1, MARGIN + 1, " ");
  }
  spinner_tick++;

  if (loading != prev_loading) {
    curs_set(loading ? 0 : 1);
    prev_loading = loading;
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
