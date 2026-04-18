#include <ncursesw/curses.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CMD_BUFFER_SIZE 32

int main(int argc, char *argv[]) {
  char input[CMD_BUFFER_SIZE] = {0};
  size_t buf_size = sizeof input;

  int pos = 0;

  initscr();
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

  mvprintw(y, x, "%s", input);

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

      if (strcmp(input, "hi") == 0) {
        memset(input, 0, buf_size);
        strcpy(input, "HELLO!");
        pos = strlen(input);
      }
    }

    // redraw
    move(y, x);
    clrtoeol();
    mvprintw(y, x, "%s", input);
  }

  endwin();
}
