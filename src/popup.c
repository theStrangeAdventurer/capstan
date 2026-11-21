#include "popup_internal.h"
#include "tui.h"
#include "utils.h"
#include "text_layout.h"
#include "clipboard.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
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
  while (*text) {
    text += text_line_length(text, max_text_w);
    if (*text == '\n') text++;
    if (*text) line_count++;
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
    int bytes = popup_text_clip(g_popup.title, popup_w - 5, 0, NULL);
    mvwprintw(win, 0, 2, " %.*s ", bytes, g_popup.title);
  }

  int item_y_offset = 1;
  if (g_popup.filterable) {
    int text_w = popup_w - 10;
    if (text_w < 1)
      text_w = 1;
    int start = popup_text_clip(g_popup.query, text_w, 1, NULL);
    mvwprintw(win, 1, 2, "Find: %s", g_popup.query + start);
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
    int bytes = popup_text_clip(g_popup.items[idx].text, text_w, 0, NULL);
    mvwprintw(win, i + item_y_offset, text_x, "%.*s", bytes,
              g_popup.items[idx].text);

    if (is_cursor)
      wattroff(win, A_REVERSE);
  }

  if (scrollbar.visible) {
    popup_draw_scrollbar(win, item_y_offset, popup_w - 2, visible, scrollbar);
  }

  if (g_popup.filterable) {
    int width;
    int limit = popup_w - 10;
    if (limit < 1) limit = 1;
    popup_text_clip(g_popup.query, limit, 1, &width);
    int x = 7 + width;
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
  g_msgpopup.text_width = max_text_w;
  g_msgpopup.content_rows = content_rows;
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
    const char *le = p + text_line_length(p, max_text_w);
    if (logical_y >= g_msgpopup.scroll) {
      size_t a = g_msgpopup.selection_start, b = g_msgpopup.selection_end;
      if (a > b) { size_t tmp = a; a = b; b = tmp; }
      mvwaddnstr(win, ty, 2, p, (int)(le - p));
      size_t start = (size_t)(p - g_msgpopup.text);
      size_t end = (size_t)(le - g_msgpopup.text);
      if (a < end && b > start) {
        size_t left = a > start ? a : start;
        size_t right = b < end ? b : end;
        int x = text_columns(p, left - start);
        int cells = text_columns(g_msgpopup.text + left, right - left);
        if (cells > 0)
          mvwchgat(win, ty, 2 + x, cells, A_REVERSE,
                   g_msgpopup.is_error ? 6 : 5, NULL);
      }
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
    const char *footer = NULL;
    if (g_msgpopup.is_error)
      footer = max_scroll > 0 ? "drag/c copy, arrows/j/k scroll, Enter close"
                              : "drag/c copy, Enter close";
    else
      footer = max_scroll > 0 ? "click URL, drag/c copy, arrows scroll, Enter close"
                              : "click URL, drag/c copy, Enter close";
    if (max_scroll > 0)
      mvwprintw(win, popup_h - 2, 2, "%.*s", footer_w, footer);
    else
      mvwprintw(win, popup_h - 2, 2, "%.*s", footer_w, footer);
    /* Keep enough room for worst-case three int values; Linux GCC treats
       possible snprintf truncation as an error under -Werror. */
    enum { POS_BUF_SIZE = 64 };
    char pos[POS_BUF_SIZE];
    snprintf(pos, sizeof(pos), "%d-%d/%d", first, last, line_count);
    int pos_len = (int)strlen(pos);
    if (pos_len < footer_w)
      mvwprintw(win, popup_h - 2, popup_w - 2 - pos_len, "%s", pos);
  }

  wnoutrefresh(win);
}

/* The desktop opener receives one literal argv value, never shell syntax.
 * Reparent it so a browser waiting for exit cannot stall the OAuth listener. */
static void popup_open_url(const char *url) {
  pid_t child = fork();
  if (child == 0) {
    pid_t opener = fork();
    if (opener < 0) _exit(1);
    if (opener > 0) _exit(0);
    setsid();
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) close(fd);
    }
#ifdef __APPLE__
    execlp("open", "open", url, (char *)NULL);
#else
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
    _exit(127);
  }
  if (child > 0)
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

int popup_message_mouse(void) {
  MEVENT event;
  if (getmouse(&event) != OK || !g_msgpopup.win) return 1;
  int y, x;
  getbegyx((WINDOW *)g_msgpopup.win, y, x);
  int row = event.y - y - 2, cell = event.x - x - 2;
  int inside = row >= 0 && row < g_msgpopup.content_rows &&
               cell >= 0 && cell < g_msgpopup.text_width;
  if (inside && (event.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)))
    return popup_message_handle_key(event.bstate & BUTTON4_PRESSED ? 'k' : 'j');
  if (!inside && !g_msgpopup.dragging) return 1;
  if (row < 0) row = 0;
  if (row >= g_msgpopup.content_rows) row = g_msgpopup.content_rows - 1;
  if (cell < 0) cell = 0;
  if (cell > g_msgpopup.text_width) cell = g_msgpopup.text_width;
  size_t offset = popup_message_offset(g_msgpopup.text, g_msgpopup.text_width,
                                       g_msgpopup.scroll + row, cell);
  if ((event.bstate & BUTTON1_PRESSED) && !g_msgpopup.dragging) {
    g_msgpopup.dragging = 1;
    g_msgpopup.selection_moved = 0;
    g_msgpopup.selection_start = g_msgpopup.selection_end = offset;
    /* Do not expire an error popup while the user is selecting it. */
    g_msgpopup.auto_close_after_ms = 0;
  }
  if (g_msgpopup.dragging) {
    g_msgpopup.selection_end = offset;
    if (offset != g_msgpopup.selection_start) g_msgpopup.selection_moved = 1;
  }
  if (!(event.bstate & (BUTTON1_RELEASED | BUTTON1_CLICKED))) return 1;
  int moved = g_msgpopup.dragging && g_msgpopup.selection_moved;
  g_msgpopup.dragging = 0;
  size_t a = g_msgpopup.selection_start, b = g_msgpopup.selection_end;
  if (moved) {
    if (a > b) { size_t tmp = a; a = b; b = tmp; }
  } else if (!inside || !popup_message_url(g_msgpopup.text, offset, &a, &b)) {
    if (inside && g_msgpopup.is_error) return popup_message_handle_key('c');
    return 1;
  }
  char *text = malloc(b - a + 1);
  if (!text) return 1;
  memcpy(text, g_msgpopup.text + a, b - a);
  text[b - a] = '\0';
  if (moved) {
    if (!getenv("CAPSTAN_DISABLE_CLIPBOARD")) clipboard_write_text(text);
  } else {
    popup_open_url(text);
  }
  free(text);
  return 1;
}
