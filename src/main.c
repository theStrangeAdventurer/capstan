#include "agent.h"
#include "http.h"
#include "plugins.h"
#include "popup.h"
#include "tui.h"
#include "utils.h"
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
static Plugin *g_popup_plugin = NULL;
static size_t g_popup_cmd_end = 0;

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

  const char *home = getenv("HOME");
  if (home) {
    char global_plugins[512];
    snprintf(global_plugins, sizeof(global_plugins),
             "%s/.config/turbo-ai/plugins", home);
    load_plugins_from(global_plugins);
  }
  load_plugins_from("plugins");

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
      if (!still_active) {
        Plugin *p = g_popup_plugin;
        size_t cmd_end = g_popup_cmd_end;
        g_popup_plugin = NULL;

        char **selected;
        int sel_count;
        selected = popup_get_selected(&sel_count);
        if (selected && sel_count > 0) {
          if (!p) {
            snprintf(g_input_buf, INPUT_BUFFER_SIZE, "%s ", selected[0]);
            g_cursor = (int)strlen(g_input_buf);
          } else {
            PluginResult *r =
                plugin_execute(p, g_input_buf, cmd_end, selected, sel_count);
            if (r) {
              const char *cmd = p->command;
              if (cmd[0] == '/')
                cmd++;
              char label[64];
              snprintf(label, sizeof(label), "ctx:%s", cmd);
              pending_add(label, r->ui_result, r->raw_result);
              free(r);
            }
            memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
            g_cursor = 0;
          }
          popup_free_selected(selected, sel_count);
        }
        popup_close();
      }
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
      if (!g_input_buf[0] && g_pending.size == 0)
        continue;

      // Real Enter — submit
      g_scroll = 0;

      char command[MAX_COMMAND_LEN];
      size_t cmd_end;
      if (g_input_buf[0] && has_command(g_input_buf, command, &cmd_end)) {
        Plugin *p = plugin_registry_find(command);
        if (p) {
          if (plugin_has_autocomplete(p)) {
            PopupItem *items;
            int count;
            char *title;
            int limit, multi;
            plugin_autocomplete_fetch(p, g_input_buf, cmd_end, &items, &count,
                                      &title, &limit, &multi);
            if (count > 0) {
              popup_open(items, count, title, limit, multi);
              free(title);
              for (int i = 0; i < count; i++) {
                free(items[i].text);
                free(items[i].value);
              }
              free(items);
              g_popup_plugin = p;
              g_popup_cmd_end = cmd_end;
              render_all();
              continue;
            }
            free(title);
            render_all();
            continue;
          } else {
            PluginResult *r = plugin_execute(p, g_input_buf, cmd_end, NULL, 0);
            if (r) {
              const char *cmd = p->command;
              if (cmd[0] == '/') cmd++;
              char label[64];
              snprintf(label, sizeof(label), "ctx:%s", cmd);
              pending_add(label, r->ui_result, r->raw_result);
              free(r);
            }
          }
        } else if (strcmp(command, "/") == 0) {
          int pc = plugin_registry_count();
          if (pc > 0) {
            PopupItem *items = malloc(pc * sizeof(PopupItem));
            for (int i = 0; i < pc; i++) {
              Plugin *pp = plugin_registry_at(i);
              char buf[256];
              snprintf(buf, sizeof(buf), "%s  %s", pp->command,
                       pp->description ? pp->description : "");
              items[i].text = my_strdup(buf);
              items[i].value = my_strdup(pp->command);
            }
            popup_open(items, pc, "Commands", 10, 0);
            for (int i = 0; i < pc; i++) {
              free(items[i].text);
              free(items[i].value);
            }
            free(items);
            g_popup_plugin = NULL;
            g_popup_cmd_end = 0;
            render_all();
            continue;
          }
          char err[256];
          snprintf(err, sizeof(err), "No plugins loaded");
          char *cp_err = my_strdup(err);
          if (g_pending.size > 0) {
            for (int i = 0; i < g_pending.size; i++) {
              add_message(g_pending.items[i].ui_result,
                         g_pending.items[i].raw_result, MSG_USER);
              g_pending.items[i].ui_result = NULL;
              g_pending.items[i].raw_result = NULL;
            }
            pending_clear();
          }
          add_message(cp_err, cp_err, MSG_USER);
          char *empty = my_strdup("");
          add_message(empty, empty, MSG_AGENT);
          agent_emit(L);
        } else {
          char err[256];
          snprintf(err, sizeof(err), "Unknown command: %s", command);
          char *cp_err = my_strdup(err);
          if (g_pending.size > 0) {
            for (int i = 0; i < g_pending.size; i++) {
              add_message(g_pending.items[i].ui_result,
                         g_pending.items[i].raw_result, MSG_USER);
              g_pending.items[i].ui_result = NULL;
              g_pending.items[i].raw_result = NULL;
            }
            pending_clear();
          }
          add_message(cp_err, cp_err, MSG_USER);
          char *empty = my_strdup("");
          add_message(empty, empty, MSG_AGENT);
          agent_emit(L);
        }
      } else if (g_pending.size > 0) {
        for (int i = 0; i < g_pending.size; i++) {
          add_message(g_pending.items[i].ui_result,
                     g_pending.items[i].raw_result, MSG_USER);
          g_pending.items[i].ui_result = NULL;
          g_pending.items[i].raw_result = NULL;
        }
        pending_clear();
        if (g_input_buf[0]) {
          char *cp = my_strdup(g_input_buf);
          add_message(cp, cp, MSG_USER);
        }
        char *empty = my_strdup("");
        add_message(empty, empty, MSG_AGENT);
        agent_emit(L);
      } else {
        char *cp_input = my_strdup(g_input_buf);
        add_message(cp_input, cp_input, MSG_USER);
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
