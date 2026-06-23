#include "popup_internal.h"
#include "tui.h"
#include <ncursesw/curses.h>
#include <string.h>

#if POPUP_KEY_UP != KEY_UP
#error "POPUP_KEY_UP != KEY_UP"
#endif
#if POPUP_KEY_DOWN != KEY_DOWN
#error "POPUP_KEY_DOWN != KEY_DOWN"
#endif
#if POPUP_KEY_LEFT != KEY_LEFT
#error "POPUP_KEY_LEFT != KEY_LEFT"
#endif
#if POPUP_KEY_RIGHT != KEY_RIGHT
#error "POPUP_KEY_RIGHT != KEY_RIGHT"
#endif

static void popup_win_destroy(void *win) { delwin((WINDOW *)win); }

void popup_init(void) { popup_set_win_cleanup(popup_win_destroy); }

void popup_close(void) {
  if (g_popup.win) {
    werase(g_popup.win);
    wnoutrefresh(g_popup.win);
    delwin(g_popup.win);
    g_popup.win = NULL;
  }
  g_popup.last_rows = -1;
  g_popup.last_cols = -1;
  popup_close_data();
}

void popup_render(void) {
  if (!g_popup.active)
    return;

  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int inner_w = cols - 2 * MARGIN;
  int input_y = rows - INPUT_WIN_HEIGHT - MARGIN;

  int popup_w = inner_w > 60 ? 60 : inner_w - 4;
  if (popup_w < POPUP_MIN_WIDTH)
    popup_w = POPUP_MIN_WIDTH;
  int visible = g_popup.item_count < g_popup.max_visible
                  ? g_popup.item_count
                  : g_popup.max_visible;
  PopupScrollbar scrollbar =
      popup_scrollbar_calc(g_popup.item_count, visible, g_popup.scroll);
  int query_rows = g_popup.filterable ? 1 : 0;
  int popup_h = visible + 2 + query_rows;
  int popup_x = MARGIN + (inner_w - popup_w) / 2;
  int popup_y = input_y - popup_h;
  if (popup_y < 0)
    popup_y = 0;

  if (!g_popup.win || g_popup.last_rows != rows || g_popup.last_cols != cols) {
    if (g_popup.win)
      delwin(g_popup.win);
    g_popup.win = newwin(popup_h, popup_w, popup_y, popup_x);
    g_popup.last_rows = rows;
    g_popup.last_cols = cols;
  }
  if (!g_popup.win)
    return;

  WINDOW *win = g_popup.win;
  werase(win);
  wbkgd(win, COLOR_PAIR(5));
  wresize(win, popup_h, popup_w);
  mvwin(win, popup_y, popup_x);

  box(win, 0, 0);
  if (g_popup.title && g_popup.title[0]) {
    int title_len = (int)strlen(g_popup.title);
    int max_title = popup_w - 4;
    if (title_len > max_title)
      title_len = max_title;
    if (title_len > 0)
      mvwprintw(win, 0, 2, " %.*s ", title_len, g_popup.title);
  }

  int item_y_offset = 1;
  if (g_popup.filterable) {
    int text_w = popup_w - 10;
    if (text_w < 1)
      text_w = 1;
    mvwprintw(win, 1, 2, "Find: %.*s", text_w, g_popup.query);
    item_y_offset = 2;
  }

  for (int i = 0; i < visible; i++) {
    int idx = g_popup.scroll + i;
    if (idx >= g_popup.item_count)
      break;

    int is_cursor = (idx == g_popup.cursor);
    int is_selected = g_popup.selected[idx];

    if (is_cursor)
      wattron(win, A_REVERSE);

    int prefix_w = popup_row_prefix_width(g_popup.multi);
    int text_x = g_popup.multi ? 1 + prefix_w : 2;
    int text_w = popup_w - text_x - (scrollbar.visible ? 2 : 1);
    if (text_w < 1)
      text_w = 1;
    if (g_popup.multi) {
      char check = is_selected ? 'x' : ' ';
      mvwprintw(win, i + item_y_offset, 1, "[%c] ", check);
    }
    mvwprintw(win, i + item_y_offset, text_x, "%.*s", text_w,
              g_popup.items[idx].text);

    if (is_cursor)
      wattroff(win, A_REVERSE);
  }

  if (scrollbar.visible) {
    int bar_x = popup_w - 2;
    for (int i = 0; i < visible; i++) {
      int ch = (i >= scrollbar.top && i < scrollbar.top + scrollbar.height)
                   ? '#'
                   : '|';
      mvwaddch(win, item_y_offset + i, bar_x, ch);
    }
  }

  if (g_popup.filterable) {
    int x = 8 + g_popup.query_len;
    if (x > popup_w - 2)
      x = popup_w - 2;
    wmove(win, 1, x);
  } else {
    wmove(win, 1 + (g_popup.cursor - g_popup.scroll),
          g_popup.multi ? 4 : 2);
  }
  wnoutrefresh(win);
}

void popup_render_message(void) {
  if (!g_msgpopup.active)
    return;

  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int inner_w = cols - 2 * MARGIN;
  int input_y = rows - INPUT_WIN_HEIGHT - MARGIN;

  int max_text_w = inner_w > 60 ? 56 : inner_w - 8;
  if (max_text_w < 20) max_text_w = 20;

  int line_count = 1;
  int line_col = 0;
  for (const char *p = g_msgpopup.text; *p; p++) {
    if (*p == '\n') { line_count++; line_col = 0; }
    else if ((*p & 0xC0) != 0x80) {
      line_col++;
      if (line_col > max_text_w) { line_count++; line_col = 1; }
    }
  }

  int popup_h = line_count + 4;
  int popup_w = max_text_w + 4;
  if (popup_w < POPUP_MIN_WIDTH) popup_w = POPUP_MIN_WIDTH;
  int popup_x = MARGIN + (inner_w - popup_w) / 2;
  int popup_y = input_y - popup_h;
  if (popup_y < 0) popup_y = 0;

  if (!g_msgpopup.win || g_msgpopup.last_rows != rows ||
      g_msgpopup.last_cols != cols) {
    if (g_msgpopup.win) delwin(g_msgpopup.win);
    g_msgpopup.win = newwin(popup_h, popup_w, popup_y, popup_x);
    g_msgpopup.last_rows = rows;
    g_msgpopup.last_cols = cols;
  }
  if (!g_msgpopup.win) return;

  WINDOW *win = g_msgpopup.win;
  werase(win);
  wbkgd(win, COLOR_PAIR(5));
  wresize(win, popup_h, popup_w);
  mvwin(win, popup_y, popup_x);

  if (g_msgpopup.is_error) {
    wattron(win, COLOR_PAIR(6));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(6));
  } else {
    box(win, 0, 0);
  }

  if (g_msgpopup.title && g_msgpopup.title[0]) {
    int title_len = (int)strlen(g_msgpopup.title);
    int max_title = popup_w - 4;
    if (title_len > max_title) title_len = max_title;
    if (title_len > 0) {
      wattron(win, A_BOLD);
      mvwprintw(win, 0, 2, " %.*s ", title_len, g_msgpopup.title);
      wattroff(win, A_BOLD);
    }
  }

  if (g_msgpopup.is_error) wattron(win, COLOR_PAIR(6));

  const char *p = g_msgpopup.text;
  int ty = 2;
  while (*p && ty < popup_h - 1) {
    const char *le = p;
    int lc = 0;
    while (*le && *le != '\n') {
      int is_char = (*le & 0xC0) != 0x80;
      if (is_char && lc >= max_text_w) break;
      le++;
      if (is_char) lc++;
    }
    mvwaddnstr(win, ty, 2, p, le - p);
    ty++;
    if (*le == '\n') le++;
    p = le;
  }

  if (g_msgpopup.is_error) wattroff(win, COLOR_PAIR(6));

  wnoutrefresh(win);
}
