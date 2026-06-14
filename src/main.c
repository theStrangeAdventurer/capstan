#include "agent.h"
#include "dispatch.h"
#include "http.h"
#include "input.h"
#include "plugins.h"
#include "popup.h"
#include "scroll.h"
#include "tui.h"
#include <lauxlib.h>
#include <locale.h>
#include <lua.h>
#include <lualib.h>
#include <ncursesw/curses.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

  const char *home = getenv("HOME");
  if (home) {
    char global_plugins[512];
    snprintf(global_plugins, sizeof(global_plugins),
             "%s/.config/turbo-ai/plugins", home);
    load_plugins_from(global_plugins);
  }
  load_plugins_from("plugins");

  input_init();
  scroll_reset();
  render_all();

  while (1) {
    int ch = getch();
    if (ch == ERR) {
      http_poll(L);
      napms(10);
      render_all();
      continue;
    }

    if (popup_is_active()) {
      int still_active = popup_handle_key(ch);
      if (!still_active)
        dispatch_popup_result();
      render_all();
      continue;
    }

    if (ch == KEY_RESIZE) {
      render_all();
      continue;
    }

    if (ch == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        if (event.bstate & BUTTON4_PRESSED)
          scroll_up(3);
        else if (event.bstate & BUTTON5_PRESSED)
          scroll_down(3);
      }
      render_all();
      continue;
    }

    if (ch == KEY_PPAGE) {
      scroll_up(5);
      render_all();
      continue;
    }

    if (ch == KEY_NPAGE) {
      scroll_down(5);
      render_all();
      continue;
    }

    if (ch == KEY_LEFT) {
      input_move_left();
      render_all();
      continue;
    }

    if (ch == KEY_RIGHT) {
      input_move_right();
      render_all();
      continue;
    }

    if (ch == '\n' || ch == '\r') {
      int next = getch();
      if (next != ERR) {
        ungetch(next);
        render_all();
        continue;
      }
      dispatch_submit();
      render_all();
      continue;
    }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      input_backspace();
    } else {
      input_insert(ch);
    }

    render_all();
  }
  endwin();
  plugin_registry_cleanup();
  system("reset");
  return 0;
}
