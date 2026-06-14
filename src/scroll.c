#include "scroll.h"

static int g_scroll = 0;

int scroll_get(void) { return g_scroll; }

void scroll_set(int val) {
  g_scroll = val < 0 ? 0 : val;
}

void scroll_up(int n) { g_scroll += n; }

void scroll_down(int n) {
  g_scroll -= n;
  if (g_scroll < 0)
    g_scroll = 0;
}

void scroll_reset(void) { g_scroll = 0; }
