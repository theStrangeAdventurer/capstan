#include "utils.h"
#include <locale.h>
#include <ncursesw/curses.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define INPUT_BUFFER_SIZE 2048

void redraw(int x, int y, char *input) {
  // redraw
  move(y, x);
  clrtoeol();
  mvprintw(y, x, "%s", input);
}

int main(int argc, char *argv[]) {
  char input[INPUT_BUFFER_SIZE] = {0};
  size_t buf_size = sizeof input;

  int pos = 0;
  setlocale(LC_ALL, ""); // Чтобы рендерились emoji
  initscr();
  noecho(); // Чтобы не выводились все символы подряд (уточнить)

  keypad(stdscr, TRUE);

  int rows, cols;

  getmaxyx(stdscr, rows, cols);

  const char *greeting =
      "Hello, World ncurses"; // Как получить ширину строки в колонках ?
                              //
  strcpy(input, greeting);
  pos = strlen(input);

  int x = cols / 2;
  int y = rows / 2;

  redraw(x, y, input);

  while (1) {
    refresh();
    int ch = getch();
    if (pos + 1 >= buf_size) {
      break;
    }

    if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8)) {
      if (pos > 0)
        input[--pos] = '\0';
    } else {
      input[pos++] = ch;
      input[pos] = '\0';

      if (strstr(input, "/hi") != NULL) {
        replace_with(input, buf_size, "/hi", "👋");
        pos = strlen(input);
      }
    }
    redraw(x, y, input);
  }

  endwin();
}
