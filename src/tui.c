#include "tui.h"
#include "agent.h"
#include "app_config.h"
#include "curses.h"
#include "http.h"
#include "input.h"
#include "linemap.h"
#include "mode.h"
#include "permit_prompt.h"
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
    init_pair(6, COLOR_RED, COLOR_BLACK);
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

static int line_contains(const char *start, const char *end, const char *needle) {
  size_t needle_len = strlen(needle);
  if (needle_len == 0)
    return 1;
  for (const char *p = start; p + needle_len <= end; p++) {
    if (strncmp(p, needle, needle_len) == 0)
      return 1;
  }
  return 0;
}

static int spinner_tick = 0;
static int prev_loading = 0;

static const char *mode_label(void) {
  if (mode_get() == FOCUS_MESSAGES) {
    if (visual_is_active())
      return " VISUAL ";
    return " MESSAGES ";
  }
  return " INSERT ";
}

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

static int centered_x(int width, const char *text) {
  int text_w = count_visible_chars(text, (int)strlen(text));
  int x = (width - text_w) / 2;
  return x > 0 ? x : 0;
}

static void render_empty_banner(WINDOW *win, int height, int width) {
  if (height < 3 || width < 1)
    return;

  int title_y = height / 2 - 1;
  int tagline_y = title_y + 1;
  int title_x = centered_x(width, APP_BANNER_TITLE);
  int tagline_x = centered_x(width, APP_BANNER_TAGLINE);

  wattron(win, A_BOLD | COLOR_PAIR(1));
  mvwaddstr(win, title_y, title_x, APP_BANNER_TITLE);
  wattroff(win, A_BOLD | COLOR_PAIR(1));

  wattron(win, A_DIM);
  mvwaddstr(win, tagline_y, tagline_x, APP_BANNER_TAGLINE);
  wattroff(win, A_DIM);
}

void render_all(void) {
  const char *input = input_get_text();
  int input_pos = input_get_cursor();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int margin = MARGIN;
  int input_h = INPUT_WIN_HEIGHT;
  int badge_h = (g_pending.size > 0 && !popup_is_active() && !popup_is_message_active()) ? 1 : 0;
  int msg_h = rows - input_h - 2 * margin - badge_h;
  int inner_w = cols - 2 * margin;
  int text_w = inner_w - 2 * MSG_PAD_H;

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
    int l = count_message_lines(msgs->items[i]->text, text_w);
    line_counts[i] = l;
    total_lines += l + 2;
  }
  scroll_update_content(total_lines, msg_h);
  int scroll_offset = scroll_get();

  const char **msgs_texts = malloc(msgs->size * sizeof(const char *));
  int *msgs_roles = malloc(msgs->size * sizeof(int));
  for (size_t i = 0; i < msgs->size; i++) {
    msgs_texts[i] = msgs->items[i]->text;
    msgs_roles[i] = msgs->items[i]->role;
  }
  linemap_build(NULL, msgs_roles, (int)msgs->size, msgs_texts, text_w);
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
  if (scroll_offset != scroll_get())
    scroll_set(scroll_offset);

  int top_line = total_lines - msg_h - scroll_offset;
  if (top_line < 0)
    top_line = 0;

  int sel_sl = -1, sel_sc = -1, sel_el = -1, sel_ec = -1;
  if (visual_is_active())
    visual_selection_range(&sel_sl, &sel_sc, &sel_el, &sel_ec);

  int global_line = 0;
  int win_row = 0;

  if (msgs->size == 0) {
    render_empty_banner(msg_win, msg_h, inner_w);
  }

  for (size_t i = 0; i < msgs->size && win_row < msg_h; i++) {
    Message *msg = msgs->items[i];
    int is_user = msg->role == MSG_USER;

    if (global_line >= top_line && win_row < msg_h) {
      if (is_user) {
        wattron(msg_win, COLOR_PAIR(5));
        mvwhline(msg_win, win_row, 0, ' ', inner_w);
        wattroff(msg_win, COLOR_PAIR(5));
      } else {
        mvwaddch(msg_win, win_row, 0, ' ');
      }
      win_row++;
    }
    global_line++;

    if (is_user)
      wattron(msg_win, COLOR_PAIR(5));
    else
      wattron(msg_win, A_DIM);

    const char *p = msg->text;
    for (int l = 0; l < line_counts[i]; l++) {
      const char *line_end = p;
      int col = 0;
      while (*line_end && *line_end != '\n') {
        int is_char = (*line_end & 0xC0) != 0x80;
        if (is_char && col >= text_w)
          break;
        line_end++;
        if (is_char)
          col++;
      }

      if (global_line >= top_line && win_row < msg_h) {
        int is_tool_status = !is_user && strncmp(p, "⚙", strlen("⚙")) == 0;
        int tool_pair = 0;
        if (is_tool_status) {
          if (line_contains(p, line_end, "done"))
            tool_pair = 2;
          else if (line_contains(p, line_end, "denied"))
            tool_pair = 6;
          wattron(msg_win, A_ITALIC);
          if (tool_pair)
            wattron(msg_win, COLOR_PAIR(tool_pair));
        }

        if (is_user)
          mvwhline(msg_win, win_row, 0, ' ', inner_w);
        mvwaddnstr(msg_win, win_row, MSG_PAD_H, p, line_end - p);

        if (is_tool_status) {
          if (tool_pair)
            wattroff(msg_win, COLOR_PAIR(tool_pair));
          wattroff(msg_win, A_ITALIC);
        }

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
            mvwchgat(msg_win, win_row, h_start + MSG_PAD_H,
                     h_end - h_start, A_REVERSE, is_user ? 5 : 0, NULL);
          }
        }

        win_row++;
      }

      if (*line_end == '\n')
        line_end++;
      p = line_end;
      global_line++;
    }

    if (is_user)
      wattroff(msg_win, COLOR_PAIR(5));
    else
      wattroff(msg_win, A_DIM);

    if (global_line >= top_line && win_row < msg_h) {
      if (is_user) {
        wattron(msg_win, COLOR_PAIR(5));
        mvwhline(msg_win, win_row, 0, ' ', inner_w);
        wattroff(msg_win, COLOR_PAIR(5));
      } else {
        mvwaddch(msg_win, win_row, 0, ' ');
      }
      win_row++;
    }
    global_line++;
  }

  free(line_counts);

  if (visual_cursor_visible()) {
    int vc_line, vc_col;
    visual_get_cursor(&vc_line, &vc_col);
    int vis_row = vc_line - top_line;
    if (vis_row >= 0 && vis_row < msg_h) {
      wattron(msg_win, A_REVERSE);
      mvwaddch(msg_win, vis_row, vc_col + MSG_PAD_H, ' ');
      wattroff(msg_win, A_REVERSE);
    }
  }

  if (g_pending.size > 0 && !popup_is_active() && !popup_is_message_active()) {
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

  {
    const char *label = mode_label();
    int label_len = (int)strlen(label);
    int label_x = 2;
    int label_attr = A_BOLD;
    if (mode_get() == FOCUS_MESSAGES && !visual_is_active())
      label_attr |= A_DIM;

    wattron(input_win, label_attr);
    if (label_x + label_len < inner_w - 1)
      mvwaddnstr(input_win, 0, label_x, label, label_len);
    wattroff(input_win, label_attr);

    UsageStats usage = agent_usage();
    if (http_is_loading() && usage.context_limit > 0 &&
        usage.total_tokens > usage.prompt_tokens) {
      usage.prompt_tokens = usage.total_tokens;
    }
    char usage_buf[32];
    int usage_len = usage_format(usage, usage_buf, sizeof(usage_buf));
    if (usage_len > 0) {
      int usage_x = inner_w - usage_len - 2;
      if (usage_x > label_x + label_len + 1) {
        wattron(input_win, A_DIM);
        mvwaddnstr(input_win, 0, usage_x, usage_buf, usage_len);
        wattroff(input_win, A_DIM);
      }
    }
  }

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
    int thinking = agent_is_thinking();
    int phase = (spinner_tick / 8) % 8;
    const char *dots[] = {" ", "·", "•", "●", "●", "•", "·", " "};
    int dot_attrs[] = {0, A_DIM, 0, A_BOLD, A_BOLD, 0, A_DIM, 0};

    wattrset(stdscr, dot_attrs[phase]);
    if (thinking)
      wattron(stdscr, COLOR_PAIR(6));
    mvaddstr(rows - 1, MARGIN + 1, dots[phase]);
    if (thinking)
      wattroff(stdscr, COLOR_PAIR(6));

    const char *label = thinking ? "Thinking" : "Answering";
    int label_attr = A_ITALIC | A_DIM;
    if (thinking)
      label_attr |= COLOR_PAIR(6);
    wattrset(stdscr, label_attr);
    mvaddstr(rows - 1, MARGIN + 1 + 5, label);
    wattrset(stdscr, A_NORMAL);
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
  popup_render_message();
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

  int choice = PERMIT_CHOICE_YES;
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
    if (permit_prompt_handle_key(ch, &choice) == PERMIT_PROMPT_DONE)
      goto done;
  }

done:
  werase(win);
  wnoutrefresh(win);
  delwin(win);

  return permit_prompt_result(choice);
}
