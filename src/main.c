#include "agent.h"
#include "http.h"
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

int g_scroll = 0;
char g_input_buf[INPUT_BUFFER_SIZE] = {0};
int g_cursor = 0;

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

  render_all();

  while (1) {
    int ch = getch();
    if (ch == ERR) {
      http_poll(L);
      napms(10);
      render_all();
      continue;
    }

    int is_enter_pressed = ch == '\n' || ch == '\r';
    int is_backspace_pressed = (ch == KEY_BACKSPACE || ch == 127 || ch == 8);

    if (ch == KEY_RESIZE) {
      render_all();
      continue;
    }

    if (ch == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        if (event.bstate & BUTTON4_PRESSED) {
          g_scroll += 3;
          render_all();
        } else if (event.bstate & BUTTON5_PRESSED) {
          g_scroll -= 3;
          if (g_scroll < 0)
            g_scroll = 0;
          render_all();
        }
      }
      continue;
    }

    if (ch == KEY_PPAGE) {
      g_scroll += 5;
      render_all();
      continue;
    }

    if (ch == KEY_NPAGE) {
      g_scroll -= 5;
      if (g_scroll < 0)
        g_scroll = 0;
      render_all();
      continue;
    }

    if (is_enter_pressed) {
      if (strlen(g_input_buf) > 0) {

        g_scroll = 0;

        char command[MAX_COMMAND_LEN];
        size_t cmd_pos;
        if (has_command(g_input_buf, command, &cmd_pos)) {
          Plugin *p = plugin_registry_find(command);
          if (p) {
            PluginResult *r = plugin_execute(p, g_input_buf);
            if (r) {
              add_message(r->ui_result, r->raw_result, MSG_USER);
            }
          } else {
            char err[256];
            snprintf(err, sizeof(err), "Unknown command: %s", command);
            add_message(err, err, MSG_USER);
          }
        } else {
          char *cp_input = my_strdup(g_input_buf);
          add_message(cp_input, cp_input, MSG_USER);
        }

        char *empty = my_strdup("");
        add_message(empty, empty, MSG_AGENT);
        agent_emit(L);
      }

      memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
      g_cursor = 0;
      render_all();
      continue;
    }

    if (is_backspace_pressed) {
      if (g_cursor > 0) {
        int prev_pos = get_prev_char_start(g_input_buf, g_cursor);
        strcpy(g_input_buf + prev_pos, g_input_buf + g_cursor);
        g_cursor = prev_pos;
      }
    } else {
      g_input_buf[g_cursor++] = ch;
      g_input_buf[g_cursor] = '\0';
    }

    render_all();
  }
  endwin();
  plugin_registry_cleanup();
  system("reset");
  return 0;
}
