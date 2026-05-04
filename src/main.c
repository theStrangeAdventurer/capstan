#include "agent.h"
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

static int has_command(const char *input, char *command, size_t *pos) {
  const char *found = strstr(input, "/");
  if (!found)
    return 0;

  *pos = found - input;

  const char *end = found;
  while (*end && *end != ' ' && *end != '\0') {
    end++;
  }

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
  int pos = 0;
  int scroll_offset = 0;

  setlocale(LC_ALL, "");
  initscr();
  noecho();
  timeout(0);
  keypad(stdscr, TRUE);
  mousemask(ALL_MOUSE_EVENTS, NULL);
  init_tui();

  plugins_init();

  struct dirent *entry;
  DIR *dir = opendir("plugins");
  if (dir) {
    while ((entry = readdir(dir)) != NULL) {
      if (strstr(entry->d_name, ".lua")) {
        char path[512];
        snprintf(path, sizeof(path), "plugins/%s", entry->d_name);
        Plugin *p = plugin_load(path);
        plugin_registry_add(p);
      }
    }
    closedir(dir);
  }

  render_all(scroll_offset, input, pos);

  while (1) {
    int ch = getch();
    if (ch == ERR) {
      napms(10);
      render_all(scroll_offset, input, pos);
      continue;
    }

    if (ch == KEY_RESIZE) {
      render_all(scroll_offset, input, pos);
      continue;
    }

    if (ch == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        if (event.bstate & BUTTON4_PRESSED) {
          scroll_offset += 3;
          render_all(scroll_offset, input, pos);
        } else if (event.bstate & BUTTON5_PRESSED) {
          scroll_offset -= 3;
          if (scroll_offset < 0)
            scroll_offset = 0;
          render_all(scroll_offset, input, pos);
        }
      }
      continue;
    }

    if (ch == KEY_PPAGE) {
      scroll_offset += 5;
      render_all(scroll_offset, input, pos);
      continue;
    }

    if (ch == KEY_NPAGE) {
      scroll_offset -= 5;
      if (scroll_offset < 0)
        scroll_offset = 0;
      render_all(scroll_offset, input, pos);
      continue;
    }

    if (ch == '\n' || ch == '\r') {
      if (strlen(input) > 0) {
        char *cp_input = my_strdup(input);

        add_message(cp_input, cp_input, MSG_USER);
        scroll_offset = 0;

        char command[MAX_COMMAND_LEN];
        size_t cmd_pos;
        if (has_command(input, command, &cmd_pos)) {
          Plugin *p = plugin_registry_find(command);
          if (p) {
            PluginResult *r = plugin_execute_sync(p, input);
            if (r) {
              add_message(r->ui_result, r->llm_result, MSG_AGENT);
            }
          }
        }
      }

      memset(input, 0, INPUT_BUFFER_SIZE);
      pos = 0;
      render_all(scroll_offset, input, pos);
      continue;
    }

    int is_backspace_pressed = (ch == KEY_BACKSPACE || ch == 127 || ch == 8);

    if (is_backspace_pressed) {
      if (pos > 0) {
        int prev_pos = get_prev_char_start(input, pos);
        strcpy(input + prev_pos, input + pos);
        pos = prev_pos;
      }
    } else {
      input[pos++] = ch;
      input[pos] = '\0';
    }

    render_all(scroll_offset, input, pos);
  }
  endwin();
  plugin_registry_cleanup();
  system("reset");
  return 0;
}
