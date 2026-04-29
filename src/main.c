#include "plugins.h"
#include "tui.h"
#include "utils.h"
#include <dirent.h>
#include <lauxlib.h>
#include <locale.h>
#include <lua.h>
#include <lualib.h>
#include <ncursesw/curses.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define INPUT_BUFFER_SIZE 8192
#define MAX_COMMAND_LEN 64

// Проверка наличия команды в строке
static int has_command(const char *input, char *command, size_t *pos) {
  const char *found = strstr(input, "/");
  if (!found)
    return 0;

  // Возвращаем позицию начала команды
  *pos = found - input;

  // Ищем конец команды (пробел или конец строки)
  const char *end = found;
  while (*end && *end != ' ' && *end != '\0') {
    end++;
  }

  // Копируем команду
  size_t cmd_len = end - found;
  if (cmd_len >= MAX_COMMAND_LEN)
    cmd_len = MAX_COMMAND_LEN - 1;
  strncpy(command, found, cmd_len);
  command[cmd_len] = '\0';

  return 1;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  char input[INPUT_BUFFER_SIZE] = {0};
  // size_t buf_size = sizeof input;

  int pos = 0;
  setlocale(LC_ALL, ""); // Чтобы рендерились emoji
  initscr();
  noecho();   // Чтобы не выводились все символы подряд
  timeout(0); // Неблокирующий getch()
  keypad(stdscr, TRUE);
  plugins_init();

  // Загружаем плагины из директории plugins/
  struct dirent *entry;
  DIR *dir = opendir("plugins");
  if (dir) {
    while ((entry = readdir(dir)) != NULL) {
      // Проверяем что это файл и имеет расширение .lua
      if (strstr(entry->d_name, ".lua")) {
        char path[512];
        snprintf(path, sizeof(path), "plugins/%s", entry->d_name);
        Plugin *p = plugin_load(path);
        plugin_registry_add(p);
      }
    }
    closedir(dir);
  }

  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  const char *greeting = "Введите команду (например: /hi, /file README.md)";
  strcpy(input, greeting);
  pos = strlen(input);

  int x = cols / 2 - strlen(greeting) / 2;
  int y = rows / 2;

  redraw(x, y, input);

  while (1) {
    refresh();

    // Обработка пользовательского ввода
    int ch = getch();
    if (ch == ERR) {
      // Нет ввода, небольшая задержка
      napms(10); // Нужно для будущей асинхронности
      redraw(x, y, input);
      continue;
    }

    if (ch == '\n' || ch == '\r') {
      // Enter - обработка команды
      char command[MAX_COMMAND_LEN];
      size_t cmd_pos;

      if (has_command(input, command, &cmd_pos)) {
        Plugin *p = plugin_registry_find(command);
        if (p) {
          char *result = plugin_execute_sync(p, input, NULL, 0);
          replace_with(input, sizeof(input), command, result);
        }
      }

      redraw(x, y, input);
      continue;
    }

    if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8)) {
      if (pos > 0) {
        input[--pos] = '\0';
      }
    } else {
      input[pos++] = ch;
      input[pos] = '\0';
    }

    redraw(x, y, input);
  }
  endwin();
  plugin_registry_cleanup();

  return 0;
}
