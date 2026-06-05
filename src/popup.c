#include "popup.h"
#include "tui.h"
#include "utils.h"
#include <ncursesw/curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  int active;
  int cancelled;

  PopupItem *items;
  int item_count;
  int *selected;

  int cursor;
  int scroll;
  int max_visible;
  char *title;
  int multi;

  WINDOW *win;
  int last_rows;
  int last_cols;
} g_popup;

void popup_open(PopupItem *items, int count, const char *title,
                int max_visible, int multi) {
  g_popup.active = 1;
  g_popup.cancelled = 0;
  g_popup.cancelled = 0;
  g_popup.cursor = 0;
  g_popup.scroll = 0;
  g_popup.max_visible = max_visible > 0 ? max_visible : POPUP_DEFAULT_LIMIT;
  g_popup.multi = multi;
  g_popup.last_rows = -1;
  g_popup.last_cols = -1;

  if (g_popup.win) {
    delwin(g_popup.win);
    g_popup.win = NULL;
  }

  free(g_popup.title);
  g_popup.title = title ? my_strdup(title) : my_strdup("");

  g_popup.item_count = count;
  g_popup.items = malloc(count * sizeof(PopupItem));
  g_popup.selected = malloc(count * sizeof(int));
  for (int i = 0; i < count; i++) {
    g_popup.items[i].text = my_strdup(items[i].text ? items[i].text : "");
    g_popup.items[i].value = my_strdup(items[i].value ? items[i].value : "");
    g_popup.selected[i] = 0;
  }
}

int popup_is_active(void) { return g_popup.active; }

int popup_handle_key(int ch) {
  switch (ch) {
  case KEY_UP:
    if (g_popup.cursor > 0) {
      g_popup.cursor--;
      if (g_popup.cursor < g_popup.scroll)
        g_popup.scroll = g_popup.cursor;
    }
    break;
  case KEY_DOWN:
    if (g_popup.cursor < g_popup.item_count - 1) {
      g_popup.cursor++;
      if (g_popup.cursor >= g_popup.scroll + g_popup.max_visible)
        g_popup.scroll = g_popup.cursor - g_popup.max_visible + 1;
    }
    break;
  case KEY_LEFT:
    if (g_popup.multi && g_popup.selected[g_popup.cursor])
      g_popup.selected[g_popup.cursor] = 0;
    break;
  case KEY_RIGHT:
    if (g_popup.multi && !g_popup.selected[g_popup.cursor])
      g_popup.selected[g_popup.cursor] = 1;
    break;
  case '\n':
  case '\r': {
    int any = 0;
    if (g_popup.multi) {
      for (int i = 0; i < g_popup.item_count; i++)
        if (g_popup.selected[i]) { any = 1; break; }
    }
    if (!any)
      g_popup.selected[g_popup.cursor] = 1;
    g_popup.cancelled = 0;
    g_popup.active = 0;
    return 0;
  }
  case 27:
    g_popup.cancelled = 1;
    g_popup.active = 0;
    return 0;
  }
  return 1;
}

char **popup_get_selected(int *count) {
  *count = 0;
  if (g_popup.cancelled)
    return NULL;
  int n = 0;
  for (int i = 0; i < g_popup.item_count; i++)
    if (g_popup.selected[i]) n++;
  if (n == 0)
    return NULL;
  char **result = malloc(n * sizeof(char *));
  int idx = 0;
  for (int i = 0; i < g_popup.item_count; i++)
    if (g_popup.selected[i])
      result[idx++] = my_strdup(g_popup.items[i].value);
  *count = n;
  return result;
}

void popup_free_selected(char **selected, int count) {
  for (int i = 0; i < count; i++)
    free(selected[i]);
  free(selected);
}

void popup_close(void) {
  for (int i = 0; i < g_popup.item_count; i++) {
    free(g_popup.items[i].text);
    free(g_popup.items[i].value);
  }
  free(g_popup.items);
  free(g_popup.selected);
  free(g_popup.title);
  if (g_popup.win) {
    werase(g_popup.win);
    wnoutrefresh(g_popup.win);
    delwin(g_popup.win);
    g_popup.win = NULL;
  }
  g_popup.items = NULL;
  g_popup.selected = NULL;
  g_popup.title = NULL;
  g_popup.item_count = 0;
  g_popup.active = 0;
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
