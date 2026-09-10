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
#include "markdown.h"
#include "text_layout.h"
#include "mode.h"
#include "permit_prompt.h"
#include "plugins.h"
#include "embedded_assets.h"
#include <lauxlib.h>
#include "popup.h"
#include "scroll.h"
#include "session_manager.h"
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
static int g_diff_add_color_pair = 0;
static int g_diff_del_color_pair = 0;

void buffer_plugin_result(const char *label, char *ui_result, char *raw_result,
                          size_t shell_output_start, MessageImage *images,
                          size_t image_count) {
  if (g_buffered_results.size >= g_buffered_results.capacity) {
    int capacity = g_buffered_results.capacity ? g_buffered_results.capacity * 2 : 4;
    BufferedPluginResult *items = realloc(g_buffered_results.items,
                                         capacity * sizeof(*items));
    if (!items) {
      free(ui_result);
      if (raw_result != ui_result) free(raw_result);
      message_images_free(images, image_count);
      popup_show_message("Command", "Cannot buffer command result: out of memory", 1);
      return;
    }
    g_buffered_results.items = items;
    g_buffered_results.capacity = capacity;
  }
  BufferedPluginResult *ctx = &g_buffered_results.items[g_buffered_results.size++];
  strncpy(ctx->label, label, MAX_BADGE_LABEL - 1);
  ctx->label[MAX_BADGE_LABEL - 1] = '\0';
  ctx->ui_result = ui_result;
  ctx->raw_result = raw_result;
  ctx->shell_output_start = shell_output_start;
  ctx->images = images;
  ctx->image_count = image_count;
}

void buffered_results_clear(void) {
  for (int i = 0; i < g_buffered_results.size; i++) {
    free(g_buffered_results.items[i].ui_result);
    if (g_buffered_results.items[i].raw_result != g_buffered_results.items[i].ui_result)
      free(g_buffered_results.items[i].raw_result);
    message_images_free(g_buffered_results.items[i].images,
                        g_buffered_results.items[i].image_count);
  }
  free(g_buffered_results.items);
  g_buffered_results.items = NULL;
  g_buffered_results.size = 0;
  g_buffered_results.capacity = 0;
}

void init_tui(void) {
  atexit(workspace_status_shutdown);
  agent_enable_shell_output(1);
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_BLACK, COLOR_YELLOW);
    init_pair(5, -1, -1);
    init_pair(21, COLOR_BLACK, COLORS >= 256 ? 234 : COLOR_BLACK);
    init_pair(6, COLOR_RED, -1);
    init_pair(8, COLOR_RED, -1);
    init_pair(9, COLORS >= 216 ? 208 : COLOR_YELLOW, -1);
    init_pair(10, COLOR_BLUE, -1);
    init_pair(11, COLOR_BLACK, COLOR_WHITE);
    /* Quiet secondary text/borders; retain attribute fallback without 256 colors. */
    init_pair(15, COLORS >= 256 ? 245 : -1, -1);
    init_pair(16, COLORS >= 256 ? 240 : -1, -1);
    /* Brand accent for status, active profile and shortcut keys. */
    init_pair(14, COLORS >= 256 ? 141 : COLOR_MAGENTA, -1);
    if (COLORS >= 256) {
      init_pair(12, 65, -1);
      init_pair(13, 95, -1);
      init_pair(20, 179, -1);
      /* Shade the original block glyphs, never their background. */
      static const short logo_colors[] = {245, 246, 247, 248, 249, 250};
      for (int i = 0; i < 6; i++)
        init_pair(22 + i, logo_colors[i], -1);
    } else {
      init_pair(12, COLOR_GREEN, -1);
      init_pair(13, COLOR_RED, -1);
      init_pair(20, COLOR_YELLOW, -1);
    }
    g_diff_add_color_pair = 12;
    g_diff_del_color_pair = 13;
  }
  curs_set(1);
}

static int spinner_tick = 0;
static int g_session_flash = -1;

/* Overlay never reserves conversation rows. */
int tui_session_height(void) { return 0; }

static int session_overlay_visible(void) {
  return agent_session_visible() &&
         tui_layout_session_height(getmaxy(stdscr), getmaxx(stdscr) - 4);
}

static const char *session_value(int row) {
  return row ? session_manager_active_id() : session_manager_active_title();
}

static void session_row(int cols, int row, TuiSessionRow *layout) {
  /* Non-Unicode locales cannot display U+29C9; use an ASCII copy mark. */
  const char *icon = MB_CUR_MAX > 1 && text_columns("⧉", strlen("⧉")) == 1 ? "⧉" : "[]";
  if (row == 1)
    tui_layout_session_id_row(cols - 4, session_value(row), icon, layout);
  else
    tui_layout_session_row(cols - 4, session_value(row), icon, layout);
  layout->x += 2; /* Reserve two extra cells for the left inset. */
}

static int session_overlay_left(int cols) {
  TuiSessionRow title, id;
  session_row(cols, 0, &title);
  session_row(cols, 1, &id);
  return (title.x < id.x ? title.x : id.x) - 3;
}

static void render_session_header(int cols) {
  if (!session_overlay_visible()) return;
  int x = session_overlay_left(cols), width = cols - 1 - x;
  WINDOW *shadow = newwin(1, width - 1, 4, x + 1);
  if (shadow) {
    wbkgd(shadow, COLOR_PAIR(21));
    werase(shadow);
    leaveok(shadow, TRUE);
    wnoutrefresh(shadow);
    delwin(shadow);
  }
  WINDOW *win = newwin(4, width, 0, x);
  if (!win) return;
  wbkgd(win, COLOR_PAIR(5));
  werase(win);
  wattron(win, A_DIM);
  box(win, 0, 0);
  wattroff(win, A_DIM);
  mvwaddstr(win, 0, width - 4, "[x]");
  for (int row = 0; row < 2; row++) {
    TuiSessionRow layout;
    session_row(cols, row, &layout);
    if (g_session_flash == row) wattron(win, A_REVERSE);
    if (layout.width) {
      const char *prefix = "session.id: ";
      size_t prefix_len = strlen(prefix);
      if (row == 1 && strncmp(layout.text, prefix, prefix_len) == 0) {
        mvwaddnstr(win, 1 + row, layout.x - x, layout.text, (int)prefix_len);
        wattron(win, A_BOLD);
        waddstr(win, layout.text + prefix_len);
        wattroff(win, A_BOLD);
      } else {
        mvwaddstr(win, 1 + row, layout.x - x, layout.text);
      }
    }
    wattroff(win, A_REVERSE);
  }
  leaveok(win, TRUE);
  wnoutrefresh(win);
  delwin(win);
}

int tui_handle_session_mouse(int y, int x, unsigned long buttons) {
  if (popup_is_active() || popup_is_message_active() ||
      !(buttons & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED))) return 0;
  int cols = getmaxx(stdscr);
  if (!session_overlay_visible() || y < 0 || y > 4 ||
      x < session_overlay_left(cols) || x >= cols) return 0;
  if (y == 0 && x >= cols - 5 && x < cols - 2) {
    if (buttons & (BUTTON1_CLICKED | BUTTON1_RELEASED))
      agent_set_session_visible(0);
    return 1;
  }
  int row = y - 1;
  if (row < 0 || row >= 2) return 1;
  TuiSessionRow layout;
  session_row(getmaxx(stdscr), row, &layout);
  if (!layout.width || x < layout.x || x >= layout.x + layout.width) return 0;
  if (buttons & (BUTTON1_CLICKED | BUTTON1_RELEASED)) {
    if (!clipboard_write_text(session_value(row))) {
      popup_show_message_ms("Clipboard", "Could not copy text", 1, 1600);
      return 1;
    }
    for (int phase = 0; phase < 3; phase++) {
      g_session_flash = phase == 1 ? -1 : row;
      render_all();
      napms(70);
    }
    g_session_flash = -1;
    popup_show_message_ms("Copied", "Text copied", 0, 500);
    render_all();
  }
  return 1;
}

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
  return A_DIM;
}

static int start_secondary_attr(void) {
  return has_colors() && COLORS >= 256 ? COLOR_PAIR(15) : A_DIM;
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
  int dim = start_secondary_attr();
  wattron(win, dim);
  mvwaddstr(win, y, x, label);
  wattroff(win, dim);
  mvwadd_clipped(win, y, x + 9, value, value_width);
}

static void render_profile_pair(WINDOW *win, int y, int x, const char *profile,
                                int value_width) {
  int dim = start_secondary_attr();
  wattron(win, dim);
  mvwaddstr(win, y, x, "profile");
  wattroff(win, dim);

  wattron(win, A_BOLD | COLOR_PAIR(14));
  mvwadd_clipped(win, y, x + 9, profile, value_width);
  wattroff(win, A_BOLD | COLOR_PAIR(14));
}

static void render_start_screen_minimal(WINDOW *win, int height, int width) {
  if (height < 3 || width < 1)
    return;

  int title_y = height / 2 - 1;
  int title_x = centered_x(width, APP_BANNER_TITLE);

  wattron(win, A_BOLD);
  mvwaddstr(win, title_y, title_x, APP_BANNER_TITLE);
  wattroff(win, A_BOLD);
}

static StartScreenAnimation g_start_animation;

static int start_screen_current_animation_tick(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0; /* Resting logo if the clock is unavailable. */
  long long now_ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
  return start_screen_animation_frame(&g_start_animation, 1, now_ms);
}

static void render_start_screen_wordmark(WINDOW *win, int y, int x) {
  int tick = start_screen_current_animation_tick();
  static const char *const cells[] = {" ", "▀", "▄", "█"};
  for (int row = 0; row < START_SCREEN_WORDMARK_DISPLAY_ROWS; row++) {
    for (int column = 0; column < START_SCREEN_WORDMARK_COLUMNS; column++) {
      int cell = start_screen_wordmark_cell(row, column);
      if (!cell)
        continue;
      int top = start_screen_wave_level(row * 2, column, tick);
      int bottom = start_screen_wave_level(row * 2 + 1, column, tick);
      int colored = has_colors() && COLORS >= 256;
      /* Attribute-only terminals retain the silhouette, at cell resolution. */
      int level = top > bottom ? top : bottom;
      int attrs = colored ? COLOR_PAIR(21 + level)
                          : level <= 2 ? A_DIM :
                            level >= 5 ? A_BOLD : A_NORMAL;
      wattron(win, attrs);
      mvwaddstr(win, y + row, x + column, cells[cell]);
      wattroff(win, attrs);
    }
  }
}

static void render_start_screen_content(WINDOW *win, int height, int width,
                                         const StartScreenStatusLines *lines) {
  StartScreenContent content = start_screen_content_for_size(height, width);
  int wide = start_screen_layout_for_size(height, width) == START_SCREEN_WIDE;
  int dim = start_secondary_attr();
  int x = content.x;
  int value_w = content.width - 9;

  if (wide) {
    render_start_screen_wordmark(win, content.y, x);
  } else {
    wattron(win, A_BOLD);
    mvwaddstr(win, content.y, x, "CAPSTAN");
    wattroff(win, A_BOLD);
  }

  /* Keep the version with the brand, not floating at the window's edge. */
  wattron(win, dim);
  int version_offset = wide ? 0 : 9;
  char version[512];
  snprintf(version, sizeof(version), "version: %s", APP_VERSION);
  mvwadd_clipped(win, content.version_y, x + version_offset, version,
                 content.width - version_offset);
  wattroff(win, dim);

  render_status_pair(win, content.status_y, x, "model", lines->model, value_w);
  render_status_pair(win, content.status_y + 1, x, "effort",
                     lines->reasoning_effort, value_w);
  render_profile_pair(win, content.status_y + 2, x, lines->profile, value_w);
  render_status_pair(win, content.status_y + 3, x, "workdir", lines->workdir,
                     value_w);

  wattron(win, dim);
  mvwadd_clipped(win, content.shortcuts_y, x, lines->shortcuts, content.width);
  wattroff(win, dim);
  mvwchgat(win, content.shortcuts_y, x, 1, A_NORMAL, 14, NULL);
  const char *key = strstr(lines->shortcuts, "Shift+Tab");
  if (key) {
    int offset = text_columns(lines->shortcuts, (size_t)(key - lines->shortcuts));
    if (offset + 9 <= content.width)
      mvwchgat(win, content.shortcuts_y, x + offset, 9, A_NORMAL, 14, NULL);
  }
}

/* Descriptor lookup only: all process I/O stays in the nonblocking collector.
 * Restore the Lua stack, including when rendering inside a tool callback. */
static struct {
  char summary[64];
  struct { char title[513], mark[8]; } items[100];
  int count, expanded, height, lines, width, y, chevron_y, chevron_x, control_width;
} g_tasks;

int tui_tasks_height(void) { return g_tasks.height; }

static int task_summary(lua_State *l) {
  lua_getglobal(l, "require");
  lua_pushliteral(l, "agent.tasks");
  lua_call(l, 1, 1);
  lua_getfield(l, -1, "view");
  lua_call(l, 0, 1);
  return 1;
}

/* Same wrapped-row traversal counts and paints: continuation titles align. */
static int task_rows(WINDOW *win, int width, int offset, int visible) {
  int row = 0;
  for (int i = 0; i < g_tasks.count; i++) {
    const char *p = g_tasks.items[i].title;
    int first = 1;
    do {
      size_t n = text_line_length(p, width);
      if (!n && *p) { int cells; n = text_character(p, strlen(p), &cells); }
      if (win && row >= offset && row < offset + visible) {
        int y = row - offset + 1;
        if (first) mvwaddstr(win, y, 1, g_tasks.items[i].mark);
        if (text_columns(p, n) <= width) mvwaddnstr(win, y, 3, p, (int)n);
        else mvwaddch(win, y, 3, '?');
      }
      row++;
      p += n;
      if (*p == '\n') p++;
      first = 0;
    } while (*p);
  }
  return row;
}

static void prepare_tasks(int available, int width) {
  memset(&g_tasks, 0, sizeof(g_tasks));
  g_tasks.chevron_y = g_tasks.chevron_x = -1;
  g_tasks.width = width;
  if (!L || width < 5) return;
  int top = lua_gettop(L);
  lua_pushcfunction(L, task_summary);
  if (lua_pcall(L, 0, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
    snprintf(g_tasks.summary, sizeof(g_tasks.summary), "Tasks: error");
    lua_settop(L, top);
    return;
  }
  lua_getfield(L, -1, "summary");
  snprintf(g_tasks.summary, sizeof(g_tasks.summary), "%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
  lua_pop(L, 1);
  lua_getfield(L, -1, "expanded");
  int manual = session_manager_tasks_view();
  g_tasks.expanded = manual ? manual == 2 : lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, -1, "items");
  if (lua_istable(L, -1)) {
    int count = (int)lua_rawlen(L, -1);
    if (count > 100) count = 100;
    for (int i = 0; i < count; i++) {
      lua_rawgeti(L, -1, i + 1);
      if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "title");
        lua_getfield(L, -2, "mark");
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
          snprintf(g_tasks.items[g_tasks.count].title, 513, "%s", lua_tostring(L, -2));
          snprintf(g_tasks.items[g_tasks.count++].mark, 8, "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 2);
      }
      lua_pop(L, 1);
    }
  }
  lua_settop(L, top);
  g_tasks.lines = task_rows(NULL, width - 4, 0, 0);
  g_tasks.height = tui_layout_tasks_height(available, g_tasks.lines, g_tasks.expanded);
  int *offset = session_manager_tasks_scroll();
  if (g_tasks.height) *offset = tui_layout_tasks_scroll(*offset, 0, g_tasks.lines, g_tasks.height - 1);
}

static void tasks_expand(int expanded) {
  if (!session_manager_set_tasks_view(expanded))
    popup_show_message("Tasks", "Could not save task view; previous state preserved", 1);
}

int tui_handle_tasks_key(int ch) {
  if (popup_is_active() || popup_is_message_active()) return 0;
  if (ch == 0x07) { /* Ctrl+G: plain BEL, no modified-arrow protocol. */
    if (g_tasks.count) tasks_expand(!g_tasks.expanded);
    return 1;
  }
  if (ch == TUI_KEY_TASKS_COLLAPSE || ch == TUI_KEY_TASKS_EXPAND) {
    tasks_expand(ch == TUI_KEY_TASKS_EXPAND);
    return 1;
  }
  if (ch != TUI_KEY_TASKS_PREV && ch != TUI_KEY_TASKS_NEXT) return 0;
  if (g_tasks.height) {
    int *offset = session_manager_tasks_scroll();
    int page = g_tasks.height - 1;
    *offset = tui_layout_tasks_scroll(*offset, ch == TUI_KEY_TASKS_PREV ? -page : page,
                                      g_tasks.lines, page);
  }
  return 1;
}

int tui_handle_tasks_mouse(int y, int x, unsigned long buttons) {
  if (popup_is_active() || popup_is_message_active()) return 0;
  if (y == g_tasks.chevron_y && x >= g_tasks.chevron_x &&
      x < g_tasks.chevron_x + g_tasks.control_width) {
    if (buttons & (BUTTON1_CLICKED | BUTTON1_RELEASED)) tasks_expand(!g_tasks.height);
    return 1;
  }
  if (!g_tasks.height || y < g_tasks.y || y >= g_tasks.y + g_tasks.height ||
      x < MARGIN || x >= MARGIN + g_tasks.width) return 0;
  int delta = buttons & BUTTON4_PRESSED ? -3 : buttons & BUTTON5_PRESSED ? 3 : 0;
  int *offset = session_manager_tasks_scroll();
  *offset = tui_layout_tasks_scroll(*offset, delta, g_tasks.lines, g_tasks.height - 1);
  return 1;
}

/* One centered label and hit area for both collapsed and expanded views. */
static void render_tasks_control(WINDOW *win, int y, int width, int clearance) {
  if (!g_tasks.summary[0]) return;
  int room = width - 2 * clearance - 6;
  if (room < 1) return;
  char summary[64], label[96];
  start_screen_truncate(g_tasks.summary, summary, sizeof(summary), room);
  snprintf(label, sizeof(label), "[ %s %s ]", summary, g_tasks.height ? "⌄" : "⌃");
  int length = text_columns(label, strlen(label));
  int x = (width - length) / 2;
  int saved = getattrs(win);
  wattrset(win, A_NORMAL);
  mvwaddstr(win, 0, x, label);
  wattrset(win, saved);
  g_tasks.chevron_y = y;
  g_tasks.chevron_x = MARGIN + x;
  g_tasks.control_width = length;
}

static void render_tasks(int input_y, int width) {
  if (!g_tasks.height) return;
  g_tasks.y = input_y - g_tasks.height;
  WINDOW *win = newwin(g_tasks.height, width, g_tasks.y, MARGIN);
  if (!win) return;
  /* Inherit the terminal palette; the input's top edge closes this panel. */
  werase(win);
  wattron(win, A_DIM);
  mvwhline(win, 0, 1, ACS_HLINE, width - 2);
  mvwvline(win, 1, 0, ACS_VLINE, g_tasks.height - 1);
  mvwvline(win, 1, width - 1, ACS_VLINE, g_tasks.height - 1);
  mvwaddstr(win, 0, 0, "╭");
  mvwaddstr(win, 0, width - 1, "╮");
  wattroff(win, A_DIM);
  render_tasks_control(win, g_tasks.y, width, 1);
  int offset = *session_manager_tasks_scroll();
  if (g_tasks.lines > g_tasks.height - 1) {
    char range[64];
    snprintf(range, sizeof(range), "%d–%d/%d", offset + 1,
             offset + g_tasks.height - 1, g_tasks.lines);
    if (text_columns(range, strlen(range)) + 2 < g_tasks.chevron_x - MARGIN) {
      wattron(win, A_DIM);
      mvwaddstr(win, 0, 1, range);
      wattroff(win, A_DIM);
    }
  }
  task_rows(win, width - 4, offset, g_tasks.height - 1);
  wnoutrefresh(win);
  delwin(win);
}

static int footer_summary_descriptor(lua_State *l) {
  lua_getglobal(l, "require");
  lua_pushliteral(l, "agent.vcs");
  lua_call(l, 1, 1);
  lua_getfield(l, -1, "summary");
  lua_call(l, 0, 4);
  return 4;
}

static void configure_workspace_footer(void) {
  if (!L) return;
  int top = lua_gettop(L);
  const char **argv = NULL;
  lua_pushcfunction(L, footer_summary_descriptor);
  if (lua_pcall(L, 0, 4, 0) != LUA_OK) goto unavailable;
  if (!lua_isstring(L, -4) || !lua_istable(L, -3) ||
      !lua_isstring(L, -2)) goto unavailable;
  size_t count = lua_rawlen(L, -3);
  if (!count || count > 256 || !lua_checkstack(L, (int)count + 1)) goto unavailable;
  argv = calloc(count + 2, sizeof(char *));
  if (!argv) goto unavailable;
  int command = lua_gettop(L) - 2;
  for (size_t i = 0; i < count; i++) {
    lua_rawgeti(L, command, (lua_Integer)i + 1);
    size_t length = 0;
    if (lua_type(L, -1) != LUA_TSTRING) goto unavailable;
    argv[i] = lua_tolstring(L, -1, &length);
    if (!length || strlen(argv[i]) != length) goto unavailable;
    /* Keep values rooted on the stack until configure copies them. */
  }
  int git_format = strcmp(lua_tostring(L, top + 3), "git") == 0;
  if (!lua_isnil(L, top + 4)) {
    if (lua_type(L, top + 4) != LUA_TSTRING) goto unavailable;
    const EmbeddedAsset *asset = embedded_asset_find(lua_tostring(L, top + 4));
    if (!asset) goto unavailable;
    argv[count] = asset->data;
  }
  workspace_status_configure(lua_tostring(L, top + 1), argv, git_format);
  free(argv);
  lua_settop(L, top);
  return;
unavailable:
  free(argv);
  workspace_status_configure(NULL, NULL, 0);
  lua_settop(L, top);
}

static void render_workspace_footer(WINDOW *win, int y, int width) {
  configure_workspace_footer();
  TuiWorkspaceFooter footer;
  tui_layout_workspace_footer(width, app_workdir(), getenv("HOME"),
                               workspace_status_poll(app_workspace_root()), &footer);
  int saved = getattrs(win);
  if (footer.path[0]) {
    wattrset(win, dim_gray_attr());
    mvwprintw(win, y, 2, " %s ", footer.path);
  }
  if (footer.summary_width) {
    int x = width - footer.summary_width - 4;
    wattrset(win, start_secondary_attr());
    mvwaddch(win, y, x++, ' ');
    mvwaddstr(win, y, x, footer.files);
    x += text_columns(footer.files, strlen(footer.files));
    wattrset(win, g_diff_add_color_pair ? COLOR_PAIR(g_diff_add_color_pair) : A_NORMAL);
    mvwaddstr(win, y, x, footer.added);
    x += text_columns(footer.added, strlen(footer.added));
    wattrset(win, g_diff_del_color_pair ? COLOR_PAIR(g_diff_del_color_pair) : A_NORMAL);
    mvwaddstr(win, y, x, footer.deleted);
    wattrset(win, A_NORMAL);
    waddch(win, ' ');
  }
  wattrset(win, saved);
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
  case START_SCREEN_COMPACT:
    render_start_screen_content(win, height, width, &lines);
    break;
  case START_SCREEN_MINIMAL:
    render_start_screen_minimal(win, height, width);
    break;
  }
}

typedef struct {
  MarkdownView markdown;
  const char *text;
} MessageView;
static MessageView *g_views;
static size_t g_view_count;
static int g_view_width;
static unsigned long g_view_revision;
static int g_views_valid;

typedef struct {
  const char *text;
  const ShellOutput *shell;
  int tool, diff;
} LiteralContext;

static int literal_message_line(size_t start, size_t end, void *data) {
  LiteralContext *ctx = data;
  const char *line = ctx->text + start;
  int len = (int)(end - start);
  if (tool_status_starts_line(line, len)) ctx->tool = 1;
  int next_diff = update_diff_state(ctx->diff, line, len);
  int raw = ctx->tool || ctx->diff || next_diff ||
            shell_output_contains(ctx->shell, start, end);
  if (tool_status_ends_line(line, len)) ctx->tool = 0;
  ctx->diff = next_diff;
  return raw;
}

static int build_message_views(Messages *messages, int width) {
  unsigned long revision = agent_messages_revision();
  if (g_views_valid && revision == g_view_revision && width == g_view_width &&
      messages->size == g_view_count) return 1;
  MessageView *views = calloc(messages->size ? messages->size : 1, sizeof(*views));
  if (!views) return 0;
  for (size_t i = 0; i < messages->size; i++) {
    Message *msg = messages->items[i];
    const char *text = shell_output_build(&msg->shell_output, msg->text);
    if (!text) text = "";
    LiteralContext context = {.text = text, .shell = &msg->shell_output};
    if (msg->role == MSG_AGENT &&
        markdown_build(&views[i].markdown, text, width, literal_message_line, &context))
      views[i].text = views[i].markdown.text;
    else
      views[i].text = text; /* Preserve raw output on allocation/parser failure. */
  }
  for (size_t i = 0; i < g_view_count; i++) markdown_free(&g_views[i].markdown);
  free(g_views);
  g_views = views;
  g_view_count = messages->size;
  g_view_width = width;
  g_view_revision = revision;
  g_views_valid = 1;
  return 1;
}

static size_t message_source(size_t message, size_t offset) {
  const MarkdownView *view = &g_views[message].markdown;
  return view->text ? (offset < view->length ? view->source[offset] : MARKDOWN_NO_SOURCE) : offset;
}

static int g_shell_top = 0, g_shell_height = 0, g_shell_width = 0;
static int g_shell_anchor_top = -1;
static unsigned long g_shell_revision = 0;

int tui_handle_shell_mouse(int y, int x, int activate) {
  if (popup_is_active() || popup_is_message_active() ||
      g_shell_revision != agent_messages_revision() ||
      y < MARGIN + tui_session_height() || y >= MARGIN + tui_session_height() + g_shell_height ||
      x < MARGIN + MSG_PAD_H || x >= MARGIN + MSG_PAD_H + g_shell_width)
    return 0;
  const LineInfo *line = linemap_get(g_shell_top + y - MARGIN - tui_session_height());
  Messages *messages = get_messages();
  if (!line || line->role == LINE_PADDING || line->msg_index >= messages->size)
    return 0;
  Message *message = messages->items[line->msg_index];
  ShellOutput *output = &message->shell_output;
  if (!output->view) return 0;
  int col = x - MARGIN - MSG_PAD_H;
  const char *text = g_views[line->msg_index].text;
  int chars = text_cell_to_char(text + line->byte_start,
                               (size_t)(line->byte_end - line->byte_start), col);
  int bytes = count_visible_chars_to(text + line->byte_start, chars);
  size_t offset = (size_t)line->byte_start + (size_t)bytes;
  if (offset >= (size_t)line->byte_end) return 0;
  offset = message_source(line->msg_index, offset);
  if (offset == MARKDOWN_NO_SOURCE) return 0;
  int block = shell_output_control(output, offset);
  if (block < 0) return 0;
  if (activate) {
    output->blocks[block].expanded = !output->blocks[block].expanded;
    g_views_valid = 0;
    g_shell_anchor_top = g_shell_top;
    visual_exit();
    render_all();
  }
  return 1;
}

void render_all(void) {
  erase(); /* Also clear overlay borders/shadow outside the chat window. */
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
  int session_h = tui_session_height();
  int msg_y = margin + session_h;
  int msg_h = rows - input_h - 2 * margin - badge_h - queue_h - session_h;
  int inner_w = cols - 2 * margin;
  int text_w = inner_w - 2 * MSG_PAD_H;
  prepare_tasks(msg_h, inner_w);
  msg_h -= g_tasks.height;

  g_shell_height = 0;
  if (msg_h < 1 || inner_w < 3 || text_w < 1)
    return;

  WINDOW *msg_win = newwin(msg_h, inner_w, msg_y, margin);
  if (!msg_win)
    return;
  werase(msg_win);

  Messages *msgs = get_messages();

  size_t count = msgs->size ? msgs->size : 1;
  int *line_counts = calloc(count, sizeof(int));
  const char **msgs_texts = malloc(count * sizeof(const char *));
  int *msgs_roles = malloc(count * sizeof(int));
  if (!line_counts || !msgs_texts || !msgs_roles || !build_message_views(msgs, text_w)) {
    free(line_counts);
    free(msgs_texts);
    free(msgs_roles);
    delwin(msg_win);
    return;
  }
  for (size_t i = 0; i < msgs->size; i++) {
    msgs_texts[i] = g_views[i].text;
    msgs_roles[i] = msgs->items[i]->role;
  }
  linemap_build(NULL, msgs_roles, (int)msgs->size, msgs_texts, text_w);
  visual_set_texts(msgs_texts, (int)msgs->size);
  free(msgs_roles);
  int total_lines = linemap_count();
  for (int line = 0; line < total_lines; line++) {
    const LineInfo *info = linemap_get(line);
    if (info->role != LINE_PADDING) line_counts[info->msg_index]++;
  }
  scroll_update_content(total_lines, msg_h);
  if (g_shell_anchor_top >= 0) {
    scroll_set(total_lines - msg_h - g_shell_anchor_top);
    g_shell_anchor_top = -1;
  }
  int scroll_offset = scroll_get();

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

  g_shell_top = top_line;
  g_shell_height = msg_h;
  g_shell_width = text_w;
  g_shell_revision = agent_messages_revision();

  int sel_sl = -1, sel_sc = -1, sel_el = -1, sel_ec = -1;
  if (visual_is_active())
    visual_selection_range(&sel_sl, &sel_sc, &sel_el, &sel_ec);

  int global_line = 0;
  int win_row = 0;

  if (msgs->size == 0) {
    /* Begin timing even in layouts that do not display the wordmark. */
    (void)start_screen_current_animation_tick();
    render_start_screen(msg_win, msg_h, inner_w);
  } else {
    (void)start_screen_animation_frame(&g_start_animation, 0, 0);
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
      wattrset(msg_win, A_NORMAL);

    const char *display_text = msgs_texts[i];
    const char *p = display_text;
    int diff_state = 0;
    const char *logical_line_start = p;
    int in_tool_status_block = 0;
    for (int l = 0; l < line_counts[i]; l++) {
      const char *line_end = p + text_line_length(p, text_w);

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
        size_t source_start = message_source(i, (size_t)(p - display_text));
        size_t source_last = line_end > p ? message_source(i, (size_t)(line_end - display_text - 1)) : source_start;
        int is_shell_output = source_start != MARKDOWN_NO_SOURCE && source_last != MARKDOWN_NO_SOURCE &&
            shell_output_contains(&msg->shell_output, source_start, source_last + 1);
        int is_tool_status = !is_user && in_tool_status_block && !is_shell_output;
        int diff_pair =
            !is_shell_output && !is_user &&
            (current_diff_state == 3 || current_diff_state == 4)
                ? diff_line_color_pair(logical_line_start)
                : 0;
        int tool_status_attrs = dim_gray_attr();
        if (is_tool_status) {
          wattroff(msg_win, A_DIM);
          wattron(msg_win, tool_status_attrs);
        }
        if (diff_pair) {
          wattron(msg_win, A_DIM | COLOR_PAIR(diff_pair));
          mvwhline(msg_win, win_row, 0, ' ', inner_w);
        }

        if (is_user) {
          mvwhline(msg_win, win_row, 0, ' ', inner_w);
          /* Gutter only: no inserted text, wrapping or copy/source offsets. */
          wattron(msg_win, start_secondary_attr());
          mvwaddstr(msg_win, win_row, 0, "│");
          wattroff(msg_win, start_secondary_attr());
        }
        if (is_shell_output)
          wattrset(msg_win, dim_gray_attr());
        wmove(msg_win, win_row, MSG_PAD_H);
        const MarkdownView *md = &g_views[i].markdown;
        int base_attrs = getattrs(msg_win);
        for (const char *run = p; run < line_end;) {
          size_t offset = (size_t)(run - display_text);
          int style = md->text ? md->styles[offset] : 0;
          const char *end = run + 1;
          while (end < line_end && (!md->text || md->styles[end - display_text] == style)) end++;
          int attrs = base_attrs;
          if (!is_shell_output && !is_tool_status) {
            if (style & MARKDOWN_BOLD) attrs = (attrs & ~A_DIM) | A_BOLD;
            if (style & MARKDOWN_ITALIC) attrs |= A_ITALIC;
          }
          wattrset(msg_win, attrs);
          waddnstr(msg_win, run, (int)(end - run));
          run = end;
        }
        wattrset(msg_win, base_attrs);
        if (is_shell_output) {
          /* Style only tagged controls, including markers split by wrapping. */
          for (const char *c = p; c < line_end; c++) {
            size_t source = message_source(i, (size_t)(c - display_text));
            if (source == MARKDOWN_NO_SOURCE || shell_output_control(&msg->shell_output, source) < 0)
              continue;
            int offset = text_columns(p, (size_t)(c - p));
            mvwchgat(msg_win, win_row, MSG_PAD_H + offset, 1,
                     A_BOLD | A_DIM, 0, NULL);
          }
        }

        if (is_shell_output)
          wattrset(msg_win, is_user ? COLOR_PAIR(5) : A_NORMAL);
        if (diff_pair)
          wattroff(msg_win, COLOR_PAIR(diff_pair));
        if (is_tool_status) {
          wattroff(msg_win, tool_status_attrs);
          wattrset(msg_win, A_NORMAL);
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
            int start_cell = text_char_to_cell(p, (size_t)(line_end - p), h_start);
            int end_cell = text_char_to_cell(p, (size_t)(line_end - p), h_end);
            mvwchgat(msg_win, win_row, start_cell + MSG_PAD_H,
                     end_cell - start_cell, A_REVERSE, is_user ? 5 : 0, NULL);
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
      const LineInfo *line = linemap_get(vc_line);
      if (line && line->role != LINE_PADDING) {
        const char *text = msgs_texts[line->msg_index] + line->byte_start;
        int cell = text_char_to_cell(text, (size_t)(line->byte_end - line->byte_start), vc_col);
        if (cell < text_w) mvwchgat(msg_win, vis_row, cell + MSG_PAD_H, 1,
                                  A_REVERSE, line->role == MSG_USER ? 5 : 0, NULL);
      }
    }
  }

  if (g_buffered_results.size > 0 && !popup_is_active() && !popup_is_message_active()) {
    int badge_y = msg_y + msg_h;
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
    int queue_y = msg_y + msg_h + badge_h;
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
      wattron(stdscr, dim_gray_attr());
      mvwadd_clipped(stdscr, queue_y + i, margin + 12, preview,
                     inner_w - 12);
      wattroff(stdscr, dim_gray_attr());
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
  int border_attr = has_colors() && COLORS >= 256 ? COLOR_PAIR(16) : A_DIM;
  wattron(input_win, border_attr);
  box(input_win, 0, 0);
  mvwaddstr(input_win, 0, 0, "╭");
  mvwaddstr(input_win, 0, inner_w - 1, "╮");
  mvwaddstr(input_win, input_h - 1, 0, "╰");
  mvwaddstr(input_win, input_h - 1, inner_w - 1, "╯");
  wattroff(input_win, border_attr);

  {
    const char *label = mode_label();
    int label_len = (int)strlen(label);
    int label_x = 2;
    int label_attr = start_secondary_attr();

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
    if (!g_tasks.height && g_tasks.summary[0]) {
      int clearance = label_x + label_len + 1;
      if (usage_len + 3 > clearance) clearance = usage_len + 3;
      render_tasks_control(input_win, input_y, inner_w, clearance);
    }
    if (usage_len > 0) {
      int usage_x = inner_w - usage_len - 2;
      if (usage_x > label_x + label_len + 1) {
        wattron(input_win, dim_gray_attr());
        mvwaddnstr(input_win, 0, usage_x, usage_buf, usage_len);
        wattroff(input_win, dim_gray_attr());
      }
    }
  }

  render_workspace_footer(input_win, input_h - 1, inner_w);

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

  if (!msgs->size && !input[0]) {
    wattron(input_win, start_secondary_attr());
    mvwadd_clipped(input_win, 1, 1, "Type a message to begin", content_w);
    wattroff(input_win, start_secondary_attr());
  }

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
  char activity_label[512] = "";
  if (loading || (activity && activity[0])) {
    const char *label = activity && activity[0] ? activity :
                        agent_is_thinking() ? "Thinking" :
                        msgs->size ? "Answering" : "Connecting";
    if (activity && activity[0]) {
      long long elapsed = agent_activity_elapsed_seconds();
      snprintf(activity_label, sizeof(activity_label), "%s · %llds", label, elapsed);
    } else {
      snprintf(activity_label, sizeof(activity_label), "%s", label);
    }
    const char *dots[] = {"·", "•", "●", "•"};
    wattrset(stdscr, COLOR_PAIR(14));
    mvaddstr(rows - 1, MARGIN + 1, dots[(spinner_tick / 8) % 4]);
  }
  TuiStatusRow status_row;
  tui_layout_status_row(cols - 2 * MARGIN - 4, activity_label,
                         agent_profile_name(), agent_provider_model(),
                         agent_reasoning_effort(), &status_row);
  wattrset(stdscr, A_NORMAL);
  mvaddstr(rows - 1, MARGIN + 3, status_row.activity);
  wattrset(stdscr, start_secondary_attr());
  if (status_row.metadata[0]) {
    int x = MARGIN + 3 + status_row.metadata_x;
    mvaddstr(rows - 1, x, status_row.metadata);
    if (status_row.profile_width)
      mvchgat(rows - 1, x, status_row.profile_width, A_BOLD, 14, NULL);
  }
  wattrset(stdscr, A_NORMAL);
  spinner_tick = (spinner_tick + 1) % 32;
  curs_set(mode_get() == FOCUS_MESSAGES ? 0 : 1);

  mvhline(0, 0, ' ', cols);
  wnoutrefresh(stdscr);
  wnoutrefresh(msg_win);
  render_tasks(input_y, inner_w);
  render_session_header(cols);
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

#if APP_KEY_SHIFT_UP != KEY_SR || APP_KEY_SHIFT_DOWN != KEY_SF
#error "Reasoning shortcut keys must match ncurses"
#endif

int tui_handle_reasoning_shortcut(int ch) {
  if (ch != APP_KEY_SHIFT_UP && ch != APP_KEY_SHIFT_DOWN)
    return 0;
  if (!L)
    return 1;
  int top = lua_gettop(L);
  const char *error = "Reasoning controls are unavailable";
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1))
    goto done;
  lua_getfield(L, -1, "agent");
  if (!lua_istable(L, -1))
    goto done;
  lua_getfield(L, -1, "step_reasoning_effort");
  if (!lua_isfunction(L, -1))
    goto done;
  lua_pushinteger(L, ch == APP_KEY_SHIFT_UP ? 1 : -1);
  if (lua_pcall(L, 1, 2, 0) != LUA_OK)
    error = lua_tostring(L, -1);
  else
    error = lua_isnil(L, -2) ? lua_tostring(L, -1) : NULL;
done:
  if (error)
    popup_show_message("Reasoning", error, 1);
  lua_settop(L, top);
  return 1;
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

static int tui_feed_paste(WINDOW *win, int ch) {
  if (ch == TUI_KEY_PASTE_BEGIN) {
    input_paste_begin();
    /* Every reader, including modal windows, must consume the body literally. */
    keypad(win, FALSE);
    return 1;
  }
  if (input_paste_feed(ch)) {
    if (!input_paste_active()) {
      keypad(win, TRUE);
      keypad(stdscr, TRUE);
    }
    return 1;
  }
  return 0;
}

int tui_handle_paste(int ch) {
  if (ch == TUI_KEY_PASTE_BEGIN && (mode_get() != FOCUS_INPUT ||
      popup_is_active() || popup_is_message_active()))
    return 0;
  return tui_feed_paste(stdscr, ch);
}

void tui_pump_blocking(void) {
  if (!stdscr)
    return;

  int ch;
  /* Bound each input batch so large pastes cannot starve the waiting work. */
  for (int count = 0; count < 256 && (ch = getch()) != ERR; count++) {
    if (tui_handle_paste(ch))
      continue;
    if (popup_is_message_active()) {
      popup_message_handle_key(ch);
      continue;
    }

    if (tui_handle_tasks_key(ch))
      continue;

    if (!popup_is_active() && tui_handle_reasoning_shortcut(ch))
      continue;

    if (mode_get() == FOCUS_INPUT && tui_handle_input_shortcut(ch))
      continue;

    if (ch == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if (tui_handle_session_mouse(event.y, event.x, event.bstate))
          continue;
        if (tui_handle_tasks_mouse(event.y, event.x, event.bstate))
          continue;
        if ((event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED)) &&
            tui_handle_shell_mouse(event.y, event.x,
                (event.bstate & (BUTTON1_CLICKED | BUTTON1_RELEASED)) != 0)) {
          continue;
        } else if ((event.bstate &
             (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED)) &&
            tui_focus_input_at_point(rows, cols, event.y, event.x)) {
          continue;
        } else if (event.bstate & BUTTON4_PRESSED) {
          visual_scroll_view(3, g_shell_height);
        } else if (event.bstate & BUTTON5_PRESSED) {
          visual_scroll_view(-3, g_shell_height);
        }
      }
      continue;
    }

    if (ch == KEY_PPAGE)
      visual_scroll_view(5, g_shell_height);
    else if (ch == KEY_NPAGE)
      visual_scroll_view(-5, g_shell_height);
    else if (mode_get() == FOCUS_MESSAGES && ch == TUI_KEY_CTRL_U)
      visual_scroll_view(g_shell_height > 1 ? g_shell_height / 2 : 1,
                         g_shell_height);
    else if (mode_get() == FOCUS_MESSAGES && ch == 0x04)
      visual_scroll_view(g_shell_height > 1 ? -g_shell_height / 2 : -1,
                         g_shell_height);
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
  keypad(win, input_paste_active() ? FALSE : TRUE);
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
    /* A paste may begin here or arrive half-consumed from the main/wait loop.
       Preserve it in the editor, never interpret it as permission shortcuts. */
    if (tui_feed_paste(win, ch))
      continue;
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
