#include "curses.h"

void redraw(int x, int y, char *input) {
  // Очищаем строку с вводом
  move(y, x);
  clrtoeol();
  mvprintw(y, x, "%s", input);
}
