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

static int has_command(const char *input, char *command, size_t *cmd_end) {
  const char *start = input;
  while (*start == ' ') start++;
  if (*start != '/')
    return 0;

  const char *end = start;
  while (*end && *end != ' ' && *end != '\0')
    end++;

  size_t len = end - start;
  if (len >= MAX_COMMAND_LEN)
    len = MAX_COMMAND_LEN - 1;
  strncpy(command, start, len);
  command[len] = '\0';
  *cmd_end = end - input;
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

    if (ch == KEY_LEFT) {
      if (g_cursor > 0)
        g_cursor = get_prev_char_start(g_input_buf, g_cursor);
      render_all();
      continue;
    }

    if (ch == KEY_RIGHT) {
      if (g_input_buf[g_cursor]) {
        g_cursor++;
        while (g_input_buf[g_cursor] && (g_input_buf[g_cursor] & 0xC0) == 0x80)
          g_cursor++;
      }
      render_all();
      continue;
    }

    // Strip newlines from pasted text: if next char is queued, it's a paste
    if (is_enter_pressed) {
      int next = getch();
      if (next != ERR) {
        ungetch(next);
        render_all();
        continue;
      }
      if (!g_input_buf[0])
        continue;

      // Real Enter — submit
      g_scroll = 0;

      char command[MAX_COMMAND_LEN];
      size_t cmd_end;
      if (has_command(g_input_buf, command, &cmd_end)) {
        Plugin *p = plugin_registry_find(command);
        if (p) {
          PluginResult *r = plugin_execute(p, g_input_buf, cmd_end);
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
