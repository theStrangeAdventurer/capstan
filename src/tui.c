#include "tui.h"
#include "agent.h"
#include "curses.h"
#include "http.h"
#include "input.h"
#include "linemap.h"
#include "mode.h"
#include "popup.h"
#include "scroll.h"
#include "utils.h"
#include "visual.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PendingContexts g_pending = {0};

void pending_add(const char *label, char *ui_result, char *raw_result) {
  if (g_pending.size >= g_pending.capacity) {
    g_pending.capacity = g_pending.capacity ? g_pending.capacity * 2 : 4;
    g_pending.items = realloc(g_pending.items,
                              g_pending.capacity * sizeof(PendingContext));
  }
  PendingContext *ctx = &g_pending.items[g_pending.size++];
  strncpy(ctx->label, label, MAX_BADGE_LABEL - 1);
  ctx->label[MAX_BADGE_LABEL - 1] = '\0';
  ctx->ui_result = ui_result;
  ctx->raw_result = raw_result;
}

void pending_clear(void) {
  for (int i = 0; i < g_pending.size; i++) {
    free(g_pending.items[i].ui_result);
    if (g_pending.items[i].raw_result != g_pending.items[i].ui_result)
      free(g_pending.items[i].raw_result);
  }
  free(g_pending.items);
  g_pending.items = NULL;
  g_pending.size = 0;
  g_pending.capacity = 0;
}

void init_tui(void) {
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_BLACK, COLOR_YELLOW);
    init_pair(5, -1, COLOR_BLACK);
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
  int scroll_offset = scroll_get();
  const char *input = input_get_text();
  int input_pos = input_get_cursor();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int margin = MARGIN;
  int input_h = INPUT_WIN_HEIGHT;
  int badge_h = (g_pending.size > 0 && !popup_is_active()) ? 1 : 0;
  int msg_h = rows - input_h - 2 * margin - badge_h;
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

  const char **msgs_texts = malloc(msgs->size * sizeof(const char *));
  int *msgs_roles = malloc(msgs->size * sizeof(int));
  for (size_t i = 0; i < msgs->size; i++) {
    msgs_texts[i] = msgs->items[i]->text;
    msgs_roles[i] = msgs->items[i]->role;
  }
  linemap_build(NULL, msgs_roles, (int)msgs->size, msgs_texts, inner_w);
  visual_set_texts(msgs_texts, (int)msgs->size);
  free(msgs_roles);

  if (visual_cursor_visible()) {
    int vc_line, vc_col;
    visual_get_cursor(&vc_line, &vc_col);
    int visual_top = total_lines - msg_h - scroll_offset;
    if (visual_top < 0) visual_top = 0;
    int visual_bottom = visual_top + msg_h - 1;
    if (vc_line < visual_top) {
      int new_scroll = total_lines - vc_line - msg_h;
      if (new_scroll < 0) new_scroll = 0;
      scroll_set(new_scroll);
      scroll_offset = scroll_get();
    } else if (vc_line > visual_bottom) {
      int new_scroll = total_lines - vc_line - 1;
      if (new_scroll < 0) new_scroll = 0;
      scroll_set(new_scroll);
      scroll_offset = scroll_get();
    }
  }

  int max_scroll = total_lines > msg_h ? total_lines - msg_h : 0;
  if (scroll_offset > max_scroll)
    scroll_offset = max_scroll;
  if (scroll_offset < 0)
    scroll_offset = 0;

  int top_line = total_lines - msg_h - scroll_offset;
  if (top_line < 0)
    top_line = 0;

  int sel_sl = -1, sel_sc = -1, sel_el = -1, sel_ec = -1;
  if (visual_is_active())
    visual_selection_range(&sel_sl, &sel_sc, &sel_el, &sel_ec);

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

        if (visual_is_active() && global_line >= sel_sl &&
            global_line <= sel_el) {
          int line_char_count = 0;
          for (const char *c = p; c < line_end; c++) {
            if ((*c & 0xC0) != 0x80)
              line_char_count++;
          }

          int h_start = (global_line == sel_sl) ? sel_sc : 0;
          int h_end = (global_line == sel_el) ? sel_ec : line_char_count;

          if (h_start < 0) h_start = 0;
          if (h_end > line_char_count) h_end = line_char_count;

          if (h_end > h_start) {
            mvwchgat(msg_win, win_row, h_start, h_end - h_start,
                     A_REVERSE, cp, NULL);
          }
        }

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

  if (visual_cursor_visible()) {
    int vc_line, vc_col;
    visual_get_cursor(&vc_line, &vc_col);
    int vis_row = vc_line - top_line;
    if (vis_row >= 0 && vis_row < msg_h) {
      wattron(msg_win, A_REVERSE);
      mvwaddch(msg_win, vis_row, vc_col, ' ');
      wattroff(msg_win, A_REVERSE);
    }
  }

  if (g_pending.size > 0 && !popup_is_active()) {
    int badge_y = margin + msg_h;
    int available = inner_w;
    int show = g_pending.size;
    int overflow = 0;

    for (int k = g_pending.size; k >= 0; k--) {
      int ov = g_pending.size - k;
      int total = 0;
      for (int i = 0; i < k; i++) {
        total += (int)strlen(g_pending.items[i].label) + 2;
        if (i > 0) total++;
      }
      if (ov > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "+%d", ov);
        total += (int)strlen(buf) + 2;
        if (k > 0) total++;
      }
      if (total <= available || k == 0) {
        show = k;
        overflow = ov;
        break;
      }
    }

    int col = margin;
    for (int i = 0; i < show; i++) {
      if (i > 0)
        mvaddch(badge_y, col++, ' ');
      wattron(stdscr, COLOR_PAIR(4));
      mvprintw(badge_y, col, " %s ", g_pending.items[i].label);
      wattroff(stdscr, COLOR_PAIR(4));
      col += strlen(g_pending.items[i].label) + 2;
    }
    if (overflow > 0) {
      if (show > 0)
        mvaddch(badge_y, col++, ' ');
      wattron(stdscr, COLOR_PAIR(4) | A_BOLD);
      mvprintw(badge_y, col, " +%d ", overflow);
      wattroff(stdscr, COLOR_PAIR(4) | A_BOLD);
    }
  }

  int input_y = rows - input_h - margin;
  WINDOW *input_win = newwin(input_h, inner_w, input_y, margin);
  if (!input_win) {
    wnoutrefresh(msg_win);
    doupdate();
    delwin(msg_win);
    return;
  }

  werase(input_win);
  if (mode_get() == FOCUS_INPUT)
    wattron(input_win, A_BOLD);
  else
    wattron(input_win, A_DIM);
  box(input_win, 0, 0);
  wattroff(input_win, A_BOLD);
  wattroff(input_win, A_DIM);

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

  int dim_content = mode_get() == FOCUS_MESSAGES;
  if (dim_content)
    wattron(input_win, A_DIM);

  int line1_bytes = 0;
  if (input_lines > skip_lines) {
    line1_bytes = count_visible_chars_to(visible, content_w);
    mvwaddnstr(input_win, 1, 1, visible, line1_bytes);
  }
  if (input_lines > skip_lines + 1)
    mvwaddstr(input_win, 2, 1, visible + line1_bytes);

  if (dim_content)
    wattroff(input_win, A_DIM);

  if (mode_get() == FOCUS_INPUT) {
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
  }

  {
    move(rows - 1, 0);
    clrtoeol();
  }

  int loading = http_is_loading();
  if (loading) {
    for (int s = 0; s < SPINNER_COUNT; s++) {
      int idx = ((spinner_tick / 4) + s * SPINNER_PHASE) % SPINNER_FRAMES;
      wattrset(stdscr, COLOR_PAIR(3) | spinner_attrs[s]);
      mvaddstr(rows - 1, MARGIN + 1 + s, spinner_frames[idx]);
    }
    wattrset(stdscr, A_NORMAL);
  }

  {
    const char *mode_hint;
    if (mode_get() == FOCUS_MESSAGES) {
      if (visual_is_active())
        mode_hint = "-- VISUAL -- y:yank  Esc:cancel";
      else
        mode_hint = "-- MESSAGES -- v:select  Esc:focus";
    } else {
      mode_hint = "-- INSERT -- Tab:focus";
    }
    int hint_len = (int)strlen(mode_hint);
    int hint_x = MARGIN + 1 + SPINNER_COUNT + 2;
    if (hint_x + hint_len < cols - 20) {
      attron(A_BOLD);
      mvaddnstr(rows - 1, hint_x, mode_hint, hint_len);
      attroff(A_BOLD);
    }
  }

  spinner_tick++;

  if (loading)
    curs_set(0);
  else if (mode_get() == FOCUS_MESSAGES)
    curs_set(0);
  else
    curs_set(1);

  if (loading != prev_loading)
    prev_loading = loading;

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
  popup_render();
  doupdate();

  delwin(msg_win);
  delwin(input_win);
}

const char *tui_permit_prompt(const char *tool, const char *target) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int popup_w = 54;
  int popup_h = 7;
  if (cols < popup_w + 4)
    popup_w = cols - 4;
  if (popup_w < 30)
    popup_w = 30;

  int popup_x = (cols - popup_w) / 2;
  int popup_y = (rows - popup_h) / 2;
  if (popup_y < 0)
    popup_y = 0;

  WINDOW *win = newwin(popup_h, popup_w, popup_y, popup_x);
  if (!win)
    return "deny";

  wattron(win, COLOR_PAIR(5));
  werase(win);
  box(win, 0, 0);
  mvwprintw(win, 0, 2, " Permit: %s ", tool);

  char target_line[256];
  snprintf(target_line, sizeof(target_line), "%.*s",
           popup_w - 6,
           target);
  mvwprintw(win, 2, 2, " %s", target_line);

  mvwaddstr(win, 4, 2, "[Y]es   [N]o   [A]lways allow");

  int choice = 0;
  const char *labels[] = {"Yes", "No", "Always"};
  int positions[] = {2, 13, 24};

  while (1) {
    for (int i = 0; i < 3; i++) {
      if (i == choice)
        wattron(win, A_REVERSE);
      mvwprintw(win, 4, positions[i], "[%c]%s",
                labels[i][0], labels[i] + 1);
      if (i == choice)
        wattroff(win, A_REVERSE);
    }
    wmove(win, 4, positions[choice]);
    wnoutrefresh(win);
    doupdate();

    int ch = wgetch(win);
    switch (ch) {
    case KEY_LEFT:
      if (choice > 0)
        choice--;
      break;
    case KEY_RIGHT:
      if (choice < 2)
        choice++;
      break;
    case 'y':
    case 'Y':
      choice = 0;
      goto done;
    case 'n':
    case 'N':
      choice = 1;
      goto done;
    case 'a':
    case 'A':
      choice = 2;
      goto done;
    case '\n':
    case '\r':
      goto done;
    case 27:
      choice = 1;
      goto done;
    }
  }

done:
  werase(win);
  wnoutrefresh(win);
  delwin(win);

  switch (choice) {
  case 0:
    return "allow";
  case 2:
    return "always";
  default:
    return "deny";
  }
}
