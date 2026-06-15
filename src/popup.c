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
  int popup_h = visible + 2;
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

  for (int i = 0; i < visible; i++) {
    int idx = g_popup.scroll + i;
    if (idx >= g_popup.item_count)
      break;

    int is_cursor = (idx == g_popup.cursor);
    int is_selected = g_popup.selected[idx];

    if (is_cursor)
      wattron(win, A_REVERSE);

    char check = is_selected ? 'x' : ' ';
    int text_w = popup_w - 6;
    mvwprintw(win, i + 1, 1, "[%c] %.*s", check, text_w,
              g_popup.items[idx].text);

    if (is_cursor)
      wattroff(win, A_REVERSE);
  }

  wmove(win, 1 + (g_popup.cursor - g_popup.scroll), 4);
  wnoutrefresh(win);
}
