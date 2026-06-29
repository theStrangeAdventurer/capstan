#include "popup_internal.h"
#include "tui.h"
#include <ncursesw/curses.h>
#include <stdio.h>
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

static int msg_wrapped_line_count(const char *text, int max_text_w) {
  int line_count = 1;
  int line_col = 0;
  for (const char *p = text; *p; p++) {
    if (*p == '\n') {
      line_count++;
      line_col = 0;
    } else if ((*p & 0xC0) != 0x80) {
      line_col++;
      if (line_col > max_text_w) {
        line_count++;
        line_col = 1;
      }
    }
  }
  return line_count;
}

static void popup_draw_scrollbar(WINDOW *win, int y, int x, int height,
                                 PopupScrollbar scrollbar) {
  if (!scrollbar.visible || height <= 0)
    return;

  wattron(win, A_DIM);
  for (int i = 0; i < height; i++)
    mvwaddch(win, y + i, x, ACS_VLINE);
  wattroff(win, A_DIM);

  if (has_colors())
    wattron(win, COLOR_PAIR(11));
  else
    wattron(win, A_REVERSE);
  for (int i = 0; i < height; i++) {
    if (i >= scrollbar.top && i < scrollbar.top + scrollbar.height)
      mvwaddch(win, y + i, x, ' ');
  }
  if (has_colors())
    wattroff(win, COLOR_PAIR(11));
  else
    wattroff(win, A_REVERSE);
}

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
    popup_draw_scrollbar(win, item_y_offset, popup_w - 2, visible, scrollbar);
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
  if (g_msgpopup.auto_close_after_ms > 0 &&
      popup_now_ms() - g_msgpopup.created_at_ms >=
          g_msgpopup.auto_close_after_ms) {
    popup_close_message();
    return;
  }

  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  if (g_msgpopup.compact) {
    const char *title = g_msgpopup.title ? g_msgpopup.title : "";
    const char *text = g_msgpopup.text ? g_msgpopup.text : "";
    int title_len = (int)strlen(title);
    int text_len = (int)strlen(text);
    int content_w = title_len > text_len ? title_len : text_len;
    int popup_w = content_w + 6;
    if (popup_w < POPUP_MIN_WIDTH)
      popup_w = POPUP_MIN_WIDTH;
    if (popup_w > cols)
      popup_w = cols;
    int popup_h = 5;
    if (popup_h > rows)
      popup_h = rows;
    int popup_x = (cols - popup_w) / 2;
    int popup_y = rows - INPUT_WIN_HEIGHT - MARGIN - popup_h - 1;
    if (popup_x < 0)
      popup_x = 0;
    if (popup_y < 0)
      popup_y = (rows - popup_h) / 2;
    if (popup_y < 0)
      popup_y = 0;

    if (!g_msgpopup.win || g_msgpopup.last_rows != rows ||
        g_msgpopup.last_cols != cols) {
      if (g_msgpopup.win)
        delwin(g_msgpopup.win);
      g_msgpopup.win = newwin(popup_h, popup_w, popup_y, popup_x);
      g_msgpopup.last_rows = rows;
      g_msgpopup.last_cols = cols;
    }
    if (!g_msgpopup.win)
      return;

    WINDOW *win = g_msgpopup.win;
    werase(win);
    wbkgd(win, COLOR_PAIR(5));
    wresize(win, popup_h, popup_w);
    mvwin(win, popup_y, popup_x);
    box(win, 0, 0);
    if (title_len > popup_w - 4)
      title_len = popup_w - 4;
    if (text_len > popup_w - 4)
      text_len = popup_w - 4;
    if (title_len > 0) {
      wattron(win, A_BOLD);
      mvwprintw(win, 1, 2, "%.*s", title_len, title);
      wattroff(win, A_BOLD);
    }
    if (text_len > 0)
      mvwprintw(win, 3, 2, "%.*s", text_len, text);
    wnoutrefresh(win);
    return;
  }

  int margin = cols >= 40 && rows >= 12 ? MARGIN : 0;
  int max_popup_w = cols - 2 * margin;
  int max_popup_h = rows - 2 * margin;
  if (max_popup_w < POPUP_MIN_WIDTH)
    max_popup_w = cols < POPUP_MIN_WIDTH ? cols : POPUP_MIN_WIDTH;
  if (max_popup_h < 5)
    max_popup_h = rows >= 5 ? rows : 5;

  int popup_w = max_popup_w;
  if (popup_w > cols)
    popup_w = cols;
  if (popup_w < POPUP_MIN_WIDTH && cols >= POPUP_MIN_WIDTH)
    popup_w = POPUP_MIN_WIDTH;

  int max_text_w = popup_w - 4;
  if (max_text_w < 1)
    max_text_w = 1;

  int line_count = msg_wrapped_line_count(g_msgpopup.text, max_text_w);
  int desired_h = line_count + 4;
  int popup_h = desired_h < max_popup_h ? desired_h : max_popup_h;
  if (popup_h < 5)
    popup_h = 5;

  int popup_x = (cols - popup_w) / 2;
  if (popup_x < 0)
    popup_x = 0;
  int popup_y = desired_h > max_popup_h ? margin : (rows - popup_h) / 2;
  if (popup_y < 0)
    popup_y = 0;

  int content_rows = popup_h - 4;
  if (content_rows < 1)
    content_rows = 1;
  int max_scroll = line_count > content_rows ? line_count - content_rows : 0;
  if (g_msgpopup.scroll < 0)
    g_msgpopup.scroll = 0;
  if (g_msgpopup.scroll > max_scroll)
    g_msgpopup.scroll = max_scroll;

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
  int logical_y = 0;
  int ty = 2;
  while (*p && ty < popup_h - 2) {
    const char *le = p;
    int lc = 0;
    while (*le && *le != '\n') {
      int is_char = (*le & 0xC0) != 0x80;
      if (is_char && lc >= max_text_w) break;
      le++;
      if (is_char) lc++;
    }
    if (logical_y >= g_msgpopup.scroll) {
      int text_w = max_text_w;
      if (max_scroll > 0)
        text_w--;
      mvwaddnstr(win, ty, 2, p, le - p > text_w ? text_w : (int)(le - p));
      ty++;
    }
    logical_y++;
    if (*le == '\n') le++;
    p = le;
  }

  if (max_scroll > 0) {
    PopupScrollbar scrollbar =
        popup_scrollbar_calc(line_count, content_rows, g_msgpopup.scroll);
    popup_draw_scrollbar(win, 2, popup_w - 2, content_rows, scrollbar);
  }

  if (g_msgpopup.is_error) wattroff(win, COLOR_PAIR(6));

  int first = line_count == 0 ? 0 : g_msgpopup.scroll + 1;
  int last = g_msgpopup.scroll + content_rows;
  if (last > line_count)
    last = line_count;
  int footer_w = popup_w - 4;
  if (footer_w > 0) {
    if (max_scroll > 0)
      mvwprintw(win, popup_h - 2, 2, "%.*s", footer_w,
                "arrows/j/k scroll, Enter close");
    else
      mvwprintw(win, popup_h - 2, 2, "%.*s", footer_w, "Enter close");
    char pos[32];
    snprintf(pos, sizeof(pos), "%d-%d/%d", first, last, line_count);
    int pos_len = (int)strlen(pos);
    if (pos_len < footer_w)
      mvwprintw(win, popup_h - 2, popup_w - 2 - pos_len, "%s", pos);
  }

  wnoutrefresh(win);
}
