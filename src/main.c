#include "agent.h"
#include "app_config.h"
#include "dispatch.h"
#include "http.h"
#include "input.h"
#include "mode.h"
#include "plugins.h"
#include "popup.h"
#include "scroll.h"
#include "tui.h"
#include "visual.h"
#include <lauxlib.h>
#include <locale.h>
#include <lua.h>
#include <lualib.h>
#include <ncursesw/curses.h>
#include <ncursesw/term.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int run_embedded_self_test(void) {
  plugins_init();
  load_embedded_plugins();

  const char *expected[] = {"/file", "/write", "/edit", "/shell", "/fetch",
                            "/logs", "/skills", "/models"};
  int ok = 1;

  printf("binary: %s\n", APP_BINARY_NAME);
  printf("plugins: %d\n", plugin_registry_count());

  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    Plugin *p = plugin_registry_find(expected[i]);
    if (!p) {
      printf("missing plugin: %s\n", expected[i]);
      ok = 0;
      continue;
    }
    printf("plugin: %s %s\n", p->command, p->id);
  }

  lua_getglobal(L, "system_prompt");
  if (!lua_isstring(L, -1) || lua_rawlen(L, -1) == 0) {
    printf("missing system_prompt\n");
    ok = 0;
  } else {
    printf("system_prompt: ok\n");
  }
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "skills_summary");
    if (lua_isstring(L, -1))
      printf("skills_summary:\n%s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  plugins_cleanup();
  return ok ? 0 : 1;
}

static int verify_terminal(void) {
  const char *term = getenv("TERM");
  if (!term || !term[0]) {
    fprintf(stderr,
            "capstan: TERM is not set; cannot initialize the terminal UI.\n");
    fprintf(stderr,
            "Set TERM to a terminal type such as xterm-256color or "
            "tmux-256color.\n");
    return 0;
  }

  int errret = 0;
  if (setupterm(NULL, STDOUT_FILENO, &errret) == ERR) {
    fprintf(stderr,
            "capstan: terminal type '%s' is not available in terminfo.\n",
            term);
    if (errret == 0) {
      fprintf(stderr,
              "No terminfo database was found. Capstan includes fallbacks for "
              "xterm-256color, tmux-256color, screen-256color, xterm, screen, "
              "ansi, and vt100.\n");
    } else if (errret == -1) {
      fprintf(stderr,
              "The terminfo database could not be opened. Check TERMINFO or "
              "TERMINFO_DIRS if you intentionally override them.\n");
    }
    fprintf(stderr,
            "Try running with TERM=xterm-256color, or rebuild Capstan with "
            "./build.sh to refresh vendored ncurses fallbacks.\n");
    return 0;
  }

  return 1;
}

int main(int argc, char *argv[]) {
  app_workdir_init(argv[0]);

  if (argc == 2 && strcmp(argv[1], "--self-test-embedded") == 0) {
    setlocale(LC_ALL, "");
    return run_embedded_self_test();
  }

  setlocale(LC_ALL, "");
  if (!verify_terminal())
    return 1;

  set_escdelay(50);
  initscr();
  noecho();
  timeout(0);
  keypad(stdscr, TRUE);
  mousemask(ALL_MOUSE_EVENTS, NULL);
  init_tui();
  popup_init();

  plugins_init();
  load_embedded_plugins();

  char global_plugins[512];
  if (app_config_path(global_plugins, sizeof(global_plugins), "plugins") == 0)
    plugins_watch_start(global_plugins);

  input_init();
  scroll_reset();
  render_all();

  while (1) {
    int ch = getch();
    if (ch == ERR) {
      http_poll(L);
      plugins_watch_poll();
      napms(10);
      render_all();
      continue;
    }

    if (popup_is_message_active()) {
      popup_message_handle_key(ch);
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

    if (mode_is_focus_toggle_key(ch)) {
      if (mode_get() == FOCUS_MESSAGES)
        visual_exit();
      mode_toggle();
      if (mode_get() == FOCUS_MESSAGES)
        visual_enter();
      render_all();
      continue;
    }

    if (ch == '\t') {
      if (mode_get() == FOCUS_INPUT)
        dispatch_tab();
      render_all();
      continue;
    }

    if (ch == ' ' && http_is_loading()) {
      if (http_cancel_streams(L) > 0) {
        agent_set_thinking(0);
        append_to_last_message("\n[stopped]\n", MSG_AGENT);
      }
      render_all();
      continue;
    }

    if (mode_get() == FOCUS_MESSAGES) {
      if (ch == KEY_PPAGE) {
        scroll_up(5);
        int vc_line;
        visual_get_cursor(&vc_line, NULL);
        visual_set_cursor_line(vc_line + 5);
      } else if (ch == KEY_NPAGE) {
        scroll_down(5);
        int vc_line;
        visual_get_cursor(&vc_line, NULL);
        visual_set_cursor_line(vc_line - 5);
      } else if (ch == '0')
        visual_move_line_start();
      else if (ch == '$')
        visual_move_line_end();
      else if (ch == 'w')
        visual_move_word_forward();
      else if (ch == 'b')
        visual_move_word_backward();
      else if (visual_is_active()) {
        if (ch == KEY_UP || ch == 'k')
          visual_move_up();
        else if (ch == KEY_DOWN || ch == 'j')
          visual_move_down();
        else if (ch == KEY_LEFT || ch == 'h')
          visual_move_left();
        else if (ch == KEY_RIGHT || ch == 'l')
          visual_move_right();
        else if (ch == 'y') {
          Messages *msgs = get_messages();
          const char **texts = malloc(msgs->size * sizeof(const char *));
          for (size_t i = 0; i < msgs->size; i++)
            texts[i] = msgs->items[i]->text;
          visual_yank(texts, (int)msgs->size);
          free(texts);
        } else if (ch == 27)
          visual_exit_selection();
      } else {
        if (ch == KEY_UP || ch == 'k')
          visual_move_up();
        else if (ch == KEY_DOWN || ch == 'j')
          visual_move_down();
        else if (ch == KEY_LEFT || ch == 'h')
          visual_move_left();
        else if (ch == KEY_RIGHT || ch == 'l')
          visual_move_right();
        else if (ch == 'v')
          visual_enter_selection();
        else if (ch == 27) {
          mode_set(FOCUS_INPUT);
          visual_exit();
        }
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
