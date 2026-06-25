#include "scroll.h"

static int g_scroll = 0;
static int g_follow_tail = 1;
static int g_has_layout = 0;
static int g_last_total_lines = 0;
static int g_last_view_height = 0;

static int max_int(int a, int b) { return a > b ? a : b; }

static int clamp_scroll(int scroll, int total_lines, int view_height) {
  int max_scroll = total_lines > view_height ? total_lines - view_height : 0;
  if (scroll < 0)
    return 0;
  if (scroll > max_scroll)
    return max_scroll;
  return scroll;
}

int scroll_get(void) { return g_scroll; }

int scroll_is_following(void) { return g_follow_tail; }

void scroll_set(int val) {
  g_scroll = val < 0 ? 0 : val;
  g_follow_tail = g_scroll == 0;
}

void scroll_up(int n) {
  if (n <= 0)
    return;
  g_scroll += n;
  g_follow_tail = 0;
}

void scroll_down(int n) {
  if (n <= 0)
    return;
  g_scroll -= n;
  if (g_scroll < 0)
    g_scroll = 0;
  if (g_scroll == 0)
    g_follow_tail = 1;
}

void scroll_reset(void) {
  g_scroll = 0;
  g_follow_tail = 1;
  g_has_layout = 0;
  g_last_total_lines = 0;
  g_last_view_height = 0;
}

void scroll_update_content(int total_lines, int view_height) {
  total_lines = max_int(total_lines, 0);
  view_height = max_int(view_height, 1);

  if (g_follow_tail) {
    g_scroll = 0;
  } else if (g_has_layout) {
    int old_top = g_last_total_lines - g_last_view_height - g_scroll;
    int old_max_top = max_int(g_last_total_lines - g_last_view_height, 0);
    if (old_top < 0)
      old_top = 0;
    if (old_top > old_max_top)
      old_top = old_max_top;
    g_scroll = total_lines - view_height - old_top;
  }

  g_scroll = clamp_scroll(g_scroll, total_lines, view_height);
  if (g_scroll == 0)
    g_follow_tail = 1;

  g_has_layout = 1;
  g_last_total_lines = total_lines;
  g_last_view_height = view_height;
}
