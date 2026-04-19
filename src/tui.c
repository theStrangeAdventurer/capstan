#include "curses.h"

void redraw(int x, int y, char *input) {
  // redraw
  move(y, x);
  clrtoeol();
  mvprintw(y, x, "%s", input);
}
