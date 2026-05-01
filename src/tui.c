#include "curses.h"

void redraw_char(int x, int y, char *input, int pos) {
  move(y, x + pos);
  printw("%c", input[pos]);
}

void redraw_backspace(int x, int y, int pos) {
  move(y, x + pos);
  printw(" "); // Напечатать пробел вместо символа
}

int count_visible_chars(const char *str, int byte_pos) {
  int chars = 0;
  for (int i = 0; i < byte_pos && str[i]; i++) {
    // UTF-8: если NOT continuation byte (не 10xxxxxx) → новый символ
    if ((str[i] & 0xC0) != 0x80) {
      chars++;
    }
  }
  return chars;
}

int get_prev_char_start(const char *str, int pos) {
  if (pos <= 0)
    return pos;

  pos--;
  // UTF-8: если бит 11xxxxxx → это начало символа
  while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
    pos--;
  }
  return pos;
}
void redraw(int x, int y, char *input) {
  // Очищаем строку с вводом
  move(y, x);
  clrtoeol();
  addstr(input); // Лучше использовать для UTF-8 символов
  // mvprintw(y, x, "%s", input);
}
