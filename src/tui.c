#include "tui.h"
#include "agent.h"
#include "app_config.h"
#include "clipboard.h"
#include "curses.h"
#include "dispatch.h"
#include "diff_highlight.h"
#include "http.h"
#include "input.h"
#include "linemap.h"
#include "mode.h"
#include "permit_prompt.h"
#include "popup.h"
#include "scroll.h"
#include "start_screen.h"
#include "tool_status.h"
#include "tui_layout.h"
#include "utils.h"
#include "visual.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

BufferedPluginResults g_buffered_results = {0};
static int g_tool_status_color_pair = 0;
static int g_dim_color_pair = 0;
static int g_diff_add_color_pair = 0;
static int g_diff_del_color_pair = 0;

void buffer_plugin_result(const char *label, char *ui_result, char *raw_result) {
  if (g_buffered_results.size >= g_buffered_results.capacity) {
    g_buffered_results.capacity = g_buffered_results.capacity ? g_buffered_results.capacity * 2 : 4;
    g_buffered_results.items = realloc(g_buffered_results.items,
                              g_buffered_results.capacity * sizeof(BufferedPluginResult));
  }
  BufferedPluginResult *ctx = &g_buffered_results.items[g_buffered_results.size++];
  strncpy(ctx->label, label, MAX_BADGE_LABEL - 1);
  ctx->label[MAX_BADGE_LABEL - 1] = '\0';
  ctx->ui_result = ui_result;
  ctx->raw_result = raw_result;
}

void buffered_results_clear(void) {
  for (int i = 0; i < g_buffered_results.size; i++) {
    free(g_buffered_results.items[i].ui_result);
    if (g_buffered_results.items[i].raw_result != g_buffered_results.items[i].ui_result)
      free(g_buffered_results.items[i].raw_result);
  }
  free(g_buffered_results.items);
  g_buffered_results.items = NULL;
  g_buffered_results.size = 0;
  g_buffered_results.capacity = 0;
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
    init_pair(8, COLOR_RED, -1);
    init_pair(9, COLORS >= 216 ? 208 : COLOR_YELLOW, -1);
    init_pair(10, COLOR_BLUE, -1);
    init_pair(11, COLOR_BLACK, COLOR_WHITE);
    if (COLORS >= 256) {
      init_pair(14, 141, -1);
      init_pair(15, 176, -1);
      init_pair(16, 177, -1);
      init_pair(17, 183, -1);
      init_pair(18, 189, -1);
      init_pair(19, 195, -1);
    } else {
      init_pair(14, COLOR_MAGENTA, -1);
      init_pair(15, COLOR_MAGENTA, -1);
      init_pair(16, COLOR_WHITE, -1);
      init_pair(17, COLOR_WHITE, -1);
      init_pair(18, COLOR_WHITE, -1);
      init_pair(19, COLOR_WHITE, -1);
    }
    if (COLORS >= 256) {
      init_pair(12, 65, -1);
      init_pair(13, 95, -1);
      init_pair(20, 179, -1);
    } else {
      init_pair(12, COLOR_GREEN, -1);
      init_pair(13, COLOR_RED, -1);
      init_pair(20, COLOR_YELLOW, -1);
    }
    g_diff_add_color_pair = 12;
    g_diff_del_color_pair = 13;
    if (COLORS > 8) {
      init_pair(7, 8, -1);
      g_tool_status_color_pair = 7;
      g_dim_color_pair = 7;
    }
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

static int dim_gray_attr(void) {
  return g_dim_color_pair ? COLOR_PAIR(g_dim_color_pair) : A_DIM;
}

static int profile_color_pair(const char *profile) {
  if (!profile)
    return 0;
  if (strcmp(profile, "implement") == 0)
    return 20;
  if (strcmp(profile, "plan") == 0)
    return 10;
  return 0;
}

static void waddn_chars(WINDOW *win, const char *text, int count) {
  for (int i = 0; i < count; i++)
    waddstr(win, text);
}

static void mvwadd_clipped(WINDOW *win, int y, int x, const char *text,
                           int max_chars) {
  if (max_chars <= 0)
    return;
  char buf[512];
  start_screen_truncate(text, buf, sizeof(buf), max_chars);
  mvwaddstr(win, y, x, buf);
}

static int diff_line_color_pair(const char *logical_line_start) {
  switch (diff_highlight_kind(logical_line_start)) {
  case DIFF_HIGHLIGHT_ADD:
    return g_diff_add_color_pair;
  case DIFF_HIGHLIGHT_DELETE:
    return g_diff_del_color_pair;
  case DIFF_HIGHLIGHT_NONE:
    return 0;
  }
  return 0;
}

static int line_starts_with(const char *line, int len, const char *prefix) {
  int prefix_len = (int)strlen(prefix);
  return len >= prefix_len && strncmp(line, prefix, prefix_len) == 0;
}

static int is_unified_diff_hunk_header(const char *line, int len) {
  if (!line_starts_with(line, len, "@@ ") || len < 5)
    return 0;
  for (int i = 3; i + 3 <= len; i++) {
    if (memcmp(line + i, " @@", 3) == 0)
      return 1;
  }
  return 0;
}

static int is_fence_line(const char *line, int len) {
  return line_starts_with(line, len, "```");
}

static int is_diff_fence_line(const char *line, int len) {
  int i = 3;
  if (!is_fence_line(line, len))
    return 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  return len - i >= 4 && strncmp(line + i, "diff", 4) == 0 &&
         (i + 4 == len || line[i + 4] == ' ' || line[i + 4] == '\t');
}

static int update_diff_state(int state, const char *line, int len) {
  if (state == 4) {
    return is_fence_line(line, len) ? 0 : 4;
  }
  if (is_diff_fence_line(line, len)) {
    return 4;
  }
  if (is_fence_line(line, len)) {
    return 0;
  }
  if (line_starts_with(line, len, "--- ")) {
    return 1;
  }
  if (state == 1 && line_starts_with(line, len, "+++ ")) {
    return 2;
  }
  if (state >= 2 && is_unified_diff_hunk_header(line, len)) {
    return 3;
  }
  if (state == 3) {
    if (len == 0 || line[0] == ' ' || line[0] == '+' || line[0] == '-' ||
        line[0] == '\\') {
      return 3;
    }
  }
  return 0;
}

static void render_status_pair(WINDOW *win, int y, int x, const char *label,
                               const char *value, int value_width) {
  int dim = dim_gray_attr();
  wattron(win, dim);
  mvwaddstr(win, y, x, label);
  wattroff(win, dim);
  wattron(win, A_BOLD);
  mvwadd_clipped(win, y, x + 9, value, value_width);
  wattroff(win, A_BOLD);
}

static void render_profile_pair(WINDOW *win, int y, int x, const char *profile,
                                int value_width) {
  int dim = dim_gray_attr();
  wattron(win, dim);
  mvwaddstr(win, y, x, "profile");
  wattroff(win, dim);

  int pair = profile_color_pair(profile);
  if (pair)
    wattron(win, A_BOLD | COLOR_PAIR(pair));
  else
    wattron(win, A_BOLD);
  mvwadd_clipped(win, y, x + 9, profile, value_width);
  if (pair)
    wattroff(win, A_BOLD | COLOR_PAIR(pair));
  else
    wattroff(win, A_BOLD);
}

static void render_start_screen_minimal(WINDOW *win, int height, int width) {
  if (height < 3 || width < 1)
    return;

  int title_y = height / 2 - 1;
  int title_x = centered_x(width, APP_BANNER_TITLE);

  wattron(win, A_BOLD | COLOR_PAIR(1));
  mvwaddstr(win, title_y, title_x, APP_BANNER_TITLE);
  wattroff(win, A_BOLD | COLOR_PAIR(1));
}

static void render_start_screen_frame(WINDOW *win, int y, int x, int frame_h,
                                      int frame_w) {
  int dim = dim_gray_attr();
  wattron(win, dim);
  mvwaddstr(win, y, x, "╭");
  waddn_chars(win, "─", frame_w - 2);
  waddstr(win, "╮");
  for (int row = 1; row < frame_h - 1; row++) {
    mvwaddstr(win, y + row, x, "│");
    mvwhline(win, y + row, x + 1, ' ', frame_w - 2);
    mvwaddstr(win, y + row, x + frame_w - 1, "│");
  }
  mvwaddstr(win, y + frame_h - 1, x, "╰");
  waddn_chars(win, "─", frame_w - 2);
  waddstr(win, "╯");
  wattroff(win, dim);
}

static int start_screen_current_animation_tick(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  long long elapsed_ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
  return start_screen_animation_tick(elapsed_ms);
}

static void render_start_screen_wordmark(WINDOW *win, int y, int x) {
  int tick = start_screen_current_animation_tick();
  for (int row = 0; row < START_SCREEN_WORDMARK_ROWS; row++) {
    for (int column = 0; column < START_SCREEN_WORDMARK_COLUMNS; column++) {
      if (!start_screen_wordmark_pixel(row, column))
        continue;
      int level = start_screen_gradient_level(row, column, tick);
      int pair = 14;
      if (level == 1 && start_screen_wordmark_grain(row, column))
        pair = 15;
      else if (level > 1)
        pair = 14 + level - 1;
      int attrs = COLOR_PAIR(pair);
      if (level == 6)
        attrs |= A_BOLD;
      wattron(win, attrs);
      mvwaddstr(win, y + row, x + column, "█");
      wattroff(win, attrs);
    }
  }
}

static void render_start_screen_wide(WINDOW *win, int height, int width,
                                     const StartScreenStatusLines *lines) {
  int frame_w = width < 88 ? width : 88;
  int frame_h = 20;
  int frame_y = (height - frame_h) / 2;
  int frame_x = (width - frame_w) / 2;
  int wordmark_w = START_SCREEN_WORDMARK_COLUMNS;
  int wordmark_x = frame_x + (frame_w - wordmark_w) / 2;
  int status_x = frame_x + 4;
  int status_right = frame_x + frame_w - 4;
  int value_w = status_right - (status_x + 9) + 1;
  int dim = dim_gray_attr();

  render_start_screen_frame(win, frame_y, frame_x, frame_h, frame_w);

  wattron(win, dim);
  mvwadd_clipped(win, frame_y + 1,
                 status_right - (int)strlen(APP_VERSION) + 1, APP_VERSION,
                 (int)strlen(APP_VERSION));
  wattroff(win, dim);

  render_start_screen_wordmark(win, frame_y + 2, wordmark_x);

  render_status_pair(win, frame_y + 9, status_x, "model", lines->model,
                     value_w);
  render_status_pair(win, frame_y + 10, status_x, "effort",
                     lines->reasoning_effort, value_w);
  render_profile_pair(win, frame_y + 11, status_x, lines->profile, value_w);
  render_status_pair(win, frame_y + 12, status_x, "workdir", lines->workdir,
                     value_w);

  wattron(win, COLOR_PAIR(2));
  mvwaddstr(win, frame_y + 15, status_x, "● ready");
  wattroff(win, COLOR_PAIR(2));
  wattron(win, dim);
  mvwadd_clipped(win, frame_y + 16, status_x, lines->ready, value_w + 9);
  wattroff(win, dim);
}

static void render_start_screen_compact(WINDOW *win, int height, int width,
                                        const StartScreenStatusLines *lines) {
  int frame_w = width < 70 ? width : 70;
  int frame_h = height < 12 ? height : 12;
  int frame_y = (height - frame_h) / 2;
  int frame_x = (width - frame_w) / 2;
  int inner_w = frame_w - 4;

  if (frame_h < 6 || frame_w < 24) {
    render_start_screen_minimal(win, height, width);
    return;
  }

  render_start_screen_frame(win, frame_y, frame_x, frame_h, frame_w);

  int dim = dim_gray_attr();
  wattron(win, dim);
  mvwadd_clipped(win, frame_y + 1,
                 frame_x + frame_w - (int)strlen(APP_VERSION) - 2,
                 APP_VERSION, (int)strlen(APP_VERSION));
  wattroff(win, dim);

  wattron(win, A_BOLD | COLOR_PAIR(1));
  mvwaddstr(win, frame_y + 2, frame_x + 2, "◉ CAPSTAN");
  wattroff(win, A_BOLD | COLOR_PAIR(1));

  render_status_pair(win, frame_y + 5, frame_x + 2, "model", lines->model,
                     inner_w - 9);
  render_status_pair(win, frame_y + 6, frame_x + 2, "effort",
                     lines->reasoning_effort, inner_w - 9);
  render_profile_pair(win, frame_y + 7, frame_x + 2, lines->profile,
                      inner_w - 9);
  render_status_pair(win, frame_y + 8, frame_x + 2, "workdir", lines->workdir,
                     inner_w - 9);

  wattron(win, COLOR_PAIR(2));
  mvwaddstr(win, frame_y + 10, frame_x + 2, "● ready");
  wattroff(win, COLOR_PAIR(2));
}

static void render_start_screen(WINDOW *win, int height, int width) {
  StartScreenStatus status = {
      .provider = agent_provider_name(),
      .model = agent_provider_model(),
      .reasoning_effort = agent_reasoning_effort(),
      .profile = agent_profile_name(),
      .workdir = app_workdir(),
  };
  StartScreenStatusLines lines;
  start_screen_build_status(&status, &lines);

  switch (start_screen_layout_for_size(height, width)) {
  case START_SCREEN_WIDE:
    render_start_screen_wide(win, height, width, &lines);
    break;
  case START_SCREEN_COMPACT:
    render_start_screen_compact(win, height, width, &lines);
    break;
  case START_SCREEN_MINIMAL:
    render_start_screen_minimal(win, height, width);
    break;
  }
}

void render_all(void) {
  const char *input = input_get_display_text();
  int input_pos = input_get_display_cursor();
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int margin = MARGIN;
  int input_h = INPUT_WIN_HEIGHT;
  int badge_h = (g_buffered_results.size > 0 && !popup_is_active() && !popup_is_message_active()) ? 1 : 0;
  int queue_h = (!popup_is_active() && !popup_is_message_active())
                    ? dispatch_queue_visible_size()
                    : 0;
  int msg_h = rows - input_h - 2 * margin - badge_h - queue_h;
  int inner_w = cols - 2 * margin;
  int text_w = inner_w - 2 * MSG_PAD_H;

  if (msg_h < 1 || inner_w < 3 || text_w < 1)
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
    render_start_screen(msg_win, msg_h, inner_w);
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
    int diff_state = 0;
    const char *logical_line_start = p;
    int in_tool_status_block = 0;
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

      int current_diff_state = diff_state;
      int next_diff_state = diff_state;
      if (*line_end == '\n' || *line_end == '\0') {
        next_diff_state = update_diff_state(
            diff_state, logical_line_start,
            (int)(line_end - logical_line_start));
      }

      int logical_line_complete = *line_end == '\n' || *line_end == '\0';
      int logical_line_len = (int)(line_end - logical_line_start);
      if (!is_user && p == logical_line_start &&
          tool_status_starts_line(logical_line_start, logical_line_len)) {
        in_tool_status_block = 1;
      }

      if (global_line >= top_line && win_row < msg_h) {
        int is_tool_status = !is_user && in_tool_status_block;
        int diff_pair =
            !is_user && (current_diff_state == 3 || current_diff_state == 4)
                ? diff_line_color_pair(logical_line_start)
                : 0;
        int tool_status_attrs = A_ITALIC;
        if (g_tool_status_color_pair)
          tool_status_attrs |= COLOR_PAIR(g_tool_status_color_pair);
        if (is_tool_status) {
          wattroff(msg_win, A_DIM);
          wattron(msg_win, tool_status_attrs);
        }
        if (diff_pair) {
          wattron(msg_win, A_DIM | COLOR_PAIR(diff_pair));
          mvwhline(msg_win, win_row, 0, ' ', inner_w);
        }

        if (is_user)
          mvwhline(msg_win, win_row, 0, ' ', inner_w);
        mvwaddnstr(msg_win, win_row, MSG_PAD_H, p, line_end - p);

        if (diff_pair)
          wattroff(msg_win, COLOR_PAIR(diff_pair));
        if (is_tool_status) {
          wattroff(msg_win, tool_status_attrs);
          wattron(msg_win, A_DIM);
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

      if (!is_user && in_tool_status_block && logical_line_complete &&
          tool_status_ends_line(logical_line_start, logical_line_len)) {
        in_tool_status_block = 0;
      }

      if (*line_end == '\n') {
        line_end++;
        logical_line_start = line_end;
      }
      p = line_end;
      diff_state = next_diff_state;
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

  if (g_buffered_results.size > 0 && !popup_is_active() && !popup_is_message_active()) {
    int badge_y = margin + msg_h;
    int available = inner_w;
    int show = g_buffered_results.size;
    int overflow = 0;

    for (int k = g_buffered_results.size; k >= 0; k--) {
      int ov = g_buffered_results.size - k;
      int total = 0;
      for (int i = 0; i < k; i++) {
        total += (int)strlen(g_buffered_results.items[i].label) + 2;
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
      mvprintw(badge_y, col, " %s ", g_buffered_results.items[i].label);
      wattroff(stdscr, COLOR_PAIR(4));
      col += strlen(g_buffered_results.items[i].label) + 2;
    }
    if (overflow > 0) {
      if (show > 0)
        mvaddch(badge_y, col++, ' ');
      wattron(stdscr, COLOR_PAIR(4) | A_BOLD);
      mvprintw(badge_y, col, " +%d ", overflow);
      wattroff(stdscr, COLOR_PAIR(4) | A_BOLD);
    }
  }

  if (queue_h > 0) {
    int queue_y = margin + msg_h + badge_h;
    for (int i = 0; i < queue_h; i++) {
      const char *queued = dispatch_queue_at(i);
      char preview[512];
      int out = 0;
      int pending_space = 0;
      for (const char *p = queued ? queued : ""; *p && out < (int)sizeof(preview) - 1;
           p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') {
          pending_space = out > 0;
          continue;
        }
        if (pending_space && out < (int)sizeof(preview) - 1)
          preview[out++] = ' ';
        pending_space = 0;
        preview[out++] = *p;
      }
      preview[out] = '\0';

      /* Queue rows live on stdscr rather than a freshly erased window. Clear
         the previous row first so a shorter preview cannot leave the tail of
         the message that previously occupied this position. */
      wmove(stdscr, queue_y + i, margin);
      wclrtoeol(stdscr);
      wattron(stdscr, dim_gray_attr());
      mvprintw(queue_y + i, margin, "queued %d/%d", i + 1,
               dispatch_queue_size());
      wattroff(stdscr, dim_gray_attr());
      wattron(stdscr, COLOR_PAIR(3));
      mvwadd_clipped(stdscr, queue_y + i, margin + 12, preview,
                     inner_w - 12);
      wattroff(stdscr, COLOR_PAIR(3));
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
  wattron(input_win, dim_gray_attr());
  box(input_win, 0, 0);
  wattroff(input_win, dim_gray_attr());

  {
    const char *label = mode_label();
    int label_len = (int)strlen(label);
    int label_x = 2;
    int label_attr = dim_gray_attr();

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
        wattron(input_win, dim_gray_attr());
        mvwaddnstr(input_win, 0, usage_x, usage_buf, usage_len);
        wattroff(input_win, dim_gray_attr());
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
    wattron(input_win, dim_gray_attr());

  int line1_bytes = 0;
  if (input_lines > skip_lines) {
    line1_bytes = count_visible_chars_to(visible, content_w);
    mvwaddnstr(input_win, 1, 1, visible, line1_bytes);
  }
  if (input_lines > skip_lines + 1)
    mvwaddstr(input_win, 2, 1, visible + line1_bytes);

  if (dim_content)
    wattroff(input_win, dim_gray_attr());

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
  const char *activity = agent_activity();
  if (loading || (activity && activity[0])) {
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

    const char *label = NULL;
    char activity_label[512];
    int label_attr = A_ITALIC | A_DIM;

    if (activity && activity[0]) {
      long long elapsed = agent_activity_elapsed_seconds();
      if (elapsed >= 60) {
        snprintf(activity_label, sizeof(activity_label), "%s · %lldm %02llds",
                 activity, elapsed / 60, elapsed % 60);
      } else {
        snprintf(activity_label, sizeof(activity_label), "%s · %llds",
                 activity, elapsed);
      }
      label = activity_label;
    } else if (thinking)
      label = "Thinking";
    else {
      /* No explicit activity label. If no user prompt has been submitted yet
         (empty history), don't say "Answering" — we're likely in startup
         (e.g. MCP init). Show a neutral label or nothing. */
      Messages *msgs = get_messages();
      int has_user = 0;
      for (size_t i = 0; i < msgs->size; i++) {
        if (msgs->items[i]->role == MSG_USER &&
            msgs->items[i]->text && msgs->items[i]->text[0]) {
          has_user = 1;
          break;
        }
      }
      if (has_user)
        label = "Answering";
    }

    if (label) {
      if (thinking)
        label_attr |= COLOR_PAIR(6);
      wattrset(stdscr, label_attr);
      mvaddstr(rows - 1, MARGIN + 1 + 5, label);
      wattrset(stdscr, A_NORMAL);
    }
  }

  spinner_tick = (spinner_tick + 1) % 64;

  if (mode_get() == FOCUS_MESSAGES)
    curs_set(0);
  else
    curs_set(1);

  const char *prov = agent_provider_name();
  const char *model = agent_provider_model();
  const char *effort = agent_reasoning_effort();
  const char *profile = agent_profile_name();
  if (prov && model) {
    char info[256];
    int n;
    if (effort && effort[0])
      n = snprintf(info, sizeof(info), "%s/%s reasoning:%s", prov, model,
                   effort);
    else
      n = snprintf(info, sizeof(info), "%s/%s", prov, model);
    if (n < 0)
      n = 0;
    info[sizeof(info) - 1] = '\0';
    const char *display = info;
    int info_len = (int)strlen(display);
    int max_width = cols > 4 ? cols - 4 : 0;
    int profile_len = profile && profile[0] ? (int)strlen(profile) : 0;
    int total_len = profile_len ? profile_len + 1 + info_len : info_len;
    if (total_len > max_width) {
      int info_max = max_width - (profile_len ? profile_len + 1 : 0);
      if (info_max < 0)
        info_max = 0;
      if (info_len > info_max) {
        display += info_len - info_max;
        info_len = info_max;
      }
      total_len = profile_len ? profile_len + 1 + info_len : info_len;
    }
    int x = cols - total_len - 2;
    if (x < 0)
      x = 0;
    if (profile_len) {
      int pair = profile_color_pair(profile);
      if (pair)
        attron(A_BOLD | COLOR_PAIR(pair));
      else
        attron(A_BOLD);
      mvaddstr(rows - 1, x, profile);
      if (pair)
        attroff(A_BOLD | COLOR_PAIR(pair));
      else
        attroff(A_BOLD);
      x += profile_len;
      attron(dim_gray_attr());
      mvaddstr(rows - 1, x, " ");
      x += 1;
      mvaddstr(rows - 1, x, display);
      attroff(dim_gray_attr());
    } else {
      attron(dim_gray_attr());
      mvaddstr(rows - 1, x, display);
      attroff(dim_gray_attr());
    }
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

void tui_paste_clipboard_image(void) {
  char error[256];
  size_t size = 0;
  unsigned char *data = clipboard_read_image(&size, error, sizeof(error));
  if (!data) {
    popup_show_message_ms("Clipboard", error[0] ? error : "No image found", 1,
                          1600);
    return;
  }
  if (size > CLIPBOARD_IMAGE_MAX_BYTES) {
    free(data);
    popup_show_message_ms("Clipboard", "Image exceeds the 10 MiB limit", 1,
                          1600);
    return;
  }
  char *base64 = clipboard_base64_encode(data, size);
  free(data);
  if (!base64 || !input_add_image("image/png", base64)) {
    free(base64);
    popup_show_message_ms("Clipboard", "Could not attach image", 1, 1600);
    return;
  }
  free(base64);
  char message[64];
  snprintf(message, sizeof(message), "Image %zu attached", input_image_count());
  popup_show_message_ms("Clipboard", message, 0, 700);
}

int tui_handle_input_shortcut(int ch) {
  if (ch == TUI_KEY_CTRL_V) {
    tui_paste_clipboard_image();
    return 1;
  }
  if (ch == TUI_KEY_CTRL_W) {
    input_delete_word_backward();
    return 1;
  }
  if (ch == TUI_KEY_CTRL_U) {
    input_delete_to_line_start();
    return 1;
  }
  return 0;
}

int tui_focus_input_at_point(int rows, int cols, int y, int x) {
  if (!tui_layout_point_in_input(rows, cols, y, x))
    return 0;
  mode_set(FOCUS_INPUT);
  visual_exit();
  return 1;
}

void tui_pump_blocking(void) {
  if (!stdscr)
    return;

  int ch;
  while ((ch = getch()) != ERR) {
    if (popup_is_message_active()) {
      popup_message_handle_key(ch);
      continue;
    }

    if (mode_get() == FOCUS_INPUT && tui_handle_input_shortcut(ch))
      continue;

    if (ch == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if ((event.bstate &
             (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED)) &&
            tui_focus_input_at_point(rows, cols, event.y, event.x)) {
          continue;
        } else if (event.bstate & BUTTON4_PRESSED) {
          scroll_up(3);
        } else if (event.bstate & BUTTON5_PRESSED) {
          scroll_down(3);
        }
      }
      continue;
    }

    if (ch == KEY_PPAGE)
      scroll_up(5);
    else if (ch == KEY_NPAGE)
      scroll_down(5);
    else if (mode_get() == FOCUS_INPUT && (ch == '\n' || ch == '\r')) {
      /* During an active top-level run dispatch_submit() can only enqueue.
         Outside that run, the blocking pump may be nested inside a Lua plugin,
         MCP operation, shell command, or permission path. Keep the editor
         contents intact instead of re-entering dispatch on the same lua_State. */
      if (dispatch_blocking_enter_allowed(agent_is_running()))
        dispatch_submit();
    }
    else if (mode_get() == FOCUS_INPUT && ch == KEY_LEFT)
      input_move_left();
    else if (mode_get() == FOCUS_INPUT && ch == KEY_RIGHT)
      input_move_right();
    else if (mode_get() == FOCUS_INPUT &&
             (ch == KEY_BACKSPACE || ch == 127 || ch == 8))
      input_backspace();
    else if (mode_get() == FOCUS_INPUT &&
             ((ch >= 0x20 && ch <= 0xFF) || ch == '\t'))
      input_insert(ch);
  }

  render_all();
}

const char *tui_permit_prompt(const char *tool, const char *target) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int popup_w = 56;
  int popup_h = 10;
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
  keypad(win, TRUE);
  nodelay(win, FALSE);

  wattron(win, COLOR_PAIR(5));
  werase(win);
  box(win, 0, 0);
  mvwprintw(win, 0, 2, " Permit: %s ", tool);

  char target_line[256];
  snprintf(target_line, sizeof(target_line), "%.*s",
           popup_w - 6,
           target);
  mvwprintw(win, 2, 2, "%s", target_line);

  int choice = PERMIT_CHOICE_ONCE;
  const char *labels[] = {"Allow once", "Allow target", "Allow tool", "Reject"};
  const char *descriptions[] = {
      "Allow this tool call",
      "Allow this target for this session",
      "Allow every target for this tool this session",
      "Deny this tool call",
  };
  const char shortcuts[] = {'Y', 'A', 'T', 'N'};
  int choice_count = 4;
  int list_y = 4;

  while (1) {
    for (int i = 0; i < choice_count; i++) {
      int y = list_y + i;
      if (i == choice)
        wattron(win, A_REVERSE);
      mvwhline(win, y, 1, ' ', popup_w - 2);
      mvwprintw(win, y, 2, "[%c] %-9s %.*s", shortcuts[i], labels[i],
                popup_w - 18, descriptions[i]);
      if (i == choice)
        wattroff(win, A_REVERSE);
    }
    wmove(win, list_y + choice, 2);
    wnoutrefresh(win);
    doupdate();

    int ch = wgetch(win);
    if (ch == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        int rel_y = event.y - popup_y;
        int rel_x = event.x - popup_x;
        int clicked = rel_y - list_y;
        if (rel_x >= 1 && rel_x < popup_w - 1 && clicked >= 0 &&
            clicked < choice_count &&
            (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED |
                             BUTTON1_RELEASED))) {
          choice = clicked;
          goto done;
        }
      }
      continue;
    }
    if (permit_prompt_handle_key(ch, &choice) == PERMIT_PROMPT_DONE)
      goto done;
  }

done:
  werase(win);
  wnoutrefresh(win);
  delwin(win);

  return permit_prompt_result(choice);
}
