#include "agent.h"
#include "app_config.h"
#include "cli_args.h"
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  char *text;
  size_t size;
  size_t cap;
  int done;
  int ok;
  char error[1024];
} HeadlessRun;

static HeadlessRun g_headless_run = {0};

#define ACTIVE_RENDER_INTERVAL_MS 16
#define IDLE_RENDER_INTERVAL_MS 50

static long long main_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void print_help(void) {
  printf("Usage:\n");
  printf("  capstan\n");
  printf("  capstan run [--prompt TEXT | --prompt-file PATH] [options]\n\n");
  printf("Options:\n");
  printf("  --provider NAME     Override provider for this run\n");
  printf("  --model ID          Override model for this run\n");
  printf("  --workdir PATH      Override workspace directory\n");
  printf("  --max-turns N       Limit agent continuation rounds (default: 200)\n");
  printf("  --no-mcp           Do not start configured MCP servers for this run\n");
  printf("  --full-control     Allow workspace-scoped tools for this run\n");
  printf("  --benchmark        Alias for --no-mcp --full-control\n");
  printf("  --json              Print structured JSON result\n");
}

static void headless_append(const char *text) {
  if (!text)
    return;
  size_t len = strlen(text);
  if (g_headless_run.size + len + 1 > g_headless_run.cap) {
    size_t next = g_headless_run.cap ? g_headless_run.cap * 2 : 1024;
    while (next < g_headless_run.size + len + 1)
      next *= 2;
    char *new_text = realloc(g_headless_run.text, next);
    if (!new_text)
      return;
    g_headless_run.text = new_text;
    g_headless_run.cap = next;
  }
  memcpy(g_headless_run.text + g_headless_run.size, text, len);
  g_headless_run.size += len;
  g_headless_run.text[g_headless_run.size] = '\0';
}

static int l_headless_on_text(lua_State *l) {
  headless_append(luaL_optstring(l, 1, ""));
  return 0;
}

static int l_headless_on_error(lua_State *l) {
  const char *message = luaL_optstring(l, 1, "unknown error");
  snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s", message);
  g_headless_run.ok = 0;
  return 0;
}

static int l_headless_on_done(lua_State *l) {
  g_headless_run.done = 1;
  g_headless_run.ok = 1;
  if (lua_istable(l, 1)) {
    lua_getfield(l, 1, "ok");
    if (lua_isboolean(l, -1) && !lua_toboolean(l, -1))
      g_headless_run.ok = 0;
    lua_pop(l, 1);

    lua_getfield(l, 1, "text");
    const char *text = lua_tostring(l, -1);
    if (text && g_headless_run.size == 0)
      headless_append(text);
    lua_pop(l, 1);

    lua_getfield(l, 1, "error");
    const char *error = lua_tostring(l, -1);
    if (error)
      snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s",
               error);
    lua_pop(l, 1);
  }
  return 0;
}

static char *read_all(FILE *f) {
  char *buf = NULL;
  size_t size = 0;
  size_t cap = 0;
  char chunk[4096];
  while (1) {
    size_t n = fread(chunk, 1, sizeof(chunk), f);
    if (n > 0) {
      if (size + n + 1 > cap) {
        size_t next = cap ? cap * 2 : 4096;
        while (next < size + n + 1)
          next *= 2;
        char *new_buf = realloc(buf, next);
        if (!new_buf) {
          free(buf);
          return NULL;
        }
        buf = new_buf;
        cap = next;
      }
      memcpy(buf + size, chunk, n);
      size += n;
    }
    if (n < sizeof(chunk)) {
      if (ferror(f)) {
        free(buf);
        return NULL;
      }
      break;
    }
  }
  if (!buf) {
    buf = malloc(1);
    if (!buf)
      return NULL;
  }
  buf[size] = '\0';
  return buf;
}

static char *read_file_prompt(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *content = read_all(f);
  fclose(f);
  return content;
}

static void json_print_string(const char *text) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;
       p++) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p == '\n') {
      printf("\\n");
    } else if (*p == '\r') {
      printf("\\r");
    } else if (*p == '\t') {
      printf("\\t");
    } else if (*p < 0x20) {
      printf("\\u%04x", *p);
    } else {
      putchar(*p);
    }
  }
  putchar('"');
}

static void print_json_result(int ok, const char *text, const char *error) {
  printf("{\"ok\":%s,\"text\":", ok ? "true" : "false");
  json_print_string(text ? text : "");
  printf(",\"error\":");
  json_print_string(error ? error : "");
  printf("}\n");
}

static int run_headless(const CliOptions *opts, const char *argv0) {
  char *owned_prompt = NULL;
  const char *prompt = opts->prompt;
  if (!prompt && opts->prompt_file) {
    owned_prompt = read_file_prompt(opts->prompt_file);
    if (!owned_prompt) {
      fprintf(stderr, "capstan: cannot read prompt file: %s\n",
              opts->prompt_file);
      return 1;
    }
    prompt = owned_prompt;
  } else if (!prompt && !isatty(STDIN_FILENO)) {
    owned_prompt = read_all(stdin);
    prompt = owned_prompt;
  }
  if (!prompt || !prompt[0]) {
    fprintf(stderr, "capstan: run requires --prompt, --prompt-file, or stdin\n");
    free(owned_prompt);
    return 1;
  }

  app_workdir_init(argv0);
  if (opts->workdir && !app_workdir_set(opts->workdir)) {
    fprintf(stderr, "capstan: invalid --workdir: %s\n", opts->workdir);
    free(owned_prompt);
    return 1;
  }

  setlocale(LC_ALL, "");
  http_set_headless(1);
  PluginsInitOptions plugin_options = {.disable_mcp = opts->no_mcp};
  plugins_init_with_options(&plugin_options);
  load_embedded_plugins();

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");

  lua_newtable(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, prompt);
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "messages");
  if (opts->provider) {
    lua_pushstring(L, opts->provider);
    lua_setfield(L, -2, "provider");
  }
  if (opts->model) {
    lua_pushstring(L, opts->model);
    lua_setfield(L, -2, "model");
  }
  lua_pushinteger(L, opts->max_turns);
  lua_setfield(L, -2, "max_turns");
  if (opts->full_control) {
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "full_control");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "workdir_only");
    lua_setfield(L, -2, "permission_scope");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "silent_tools");
  }

  lua_newtable(L);
  lua_pushcfunction(L, l_headless_on_text);
  lua_setfield(L, -2, "on_text");
  lua_pushcfunction(L, l_headless_on_error);
  lua_setfield(L, -2, "on_error");
  lua_pushcfunction(L, l_headless_on_done);
  lua_setfield(L, -2, "on_done");

  if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
    const char *message = lua_tostring(L, -1);
    if (opts->json)
      print_json_result(0, "", message);
    else
      fprintf(stderr, "capstan: %s\n", message);
    lua_pop(L, 1);
    plugins_cleanup();
    free(owned_prompt);
    return 1;
  }
  int started = lua_toboolean(L, -2);
  const char *start_err = lua_tostring(L, -1);
  lua_pop(L, 4);
  if (!started) {
    const char *message = start_err ? start_err : "run failed";
    if (opts->json)
      print_json_result(0, "", message);
    else
      fprintf(stderr, "capstan: %s\n", message);
    plugins_cleanup();
    free(owned_prompt);
    return 1;
  }

  while (!g_headless_run.done && http_is_loading()) {
    http_poll(L);
    usleep(10000);
  }

  if (!g_headless_run.done && !g_headless_run.error[0]) {
    snprintf(g_headless_run.error, sizeof(g_headless_run.error),
             "agent stream ended without completion");
    g_headless_run.ok = 0;
  }

  if (opts->json) {
    print_json_result(g_headless_run.ok, g_headless_run.text,
                      g_headless_run.error);
  } else if (g_headless_run.ok) {
    printf("%s", g_headless_run.text ? g_headless_run.text : "");
    if (!g_headless_run.text || g_headless_run.size == 0 ||
        g_headless_run.text[g_headless_run.size - 1] != '\n')
      putchar('\n');
  } else {
    fprintf(stderr, "capstan: %s\n",
            g_headless_run.error[0] ? g_headless_run.error : "run failed");
  }

  int rc = g_headless_run.ok ? 0 : 1;
  free(g_headless_run.text);
  g_headless_run = (HeadlessRun){0};
  plugins_cleanup();
  free(owned_prompt);
  return rc;
}

static int run_embedded_self_test(void) {
  plugins_init();
  load_embedded_plugins();

  const char *expected[] = {"/file", "/write", "/edit", "/shell", "/fetch",
                            "/logs", "/skills", "/models", "/info", "/mcp"};
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
  CliOptions cli = cli_parse(argc, argv);

  if (cli.mode == CLI_MODE_ERROR) {
    fprintf(stderr, "capstan: %s\n", cli.error ? cli.error : "invalid args");
    print_help();
    return 2;
  }

  if (cli.mode == CLI_MODE_HELP) {
    print_help();
    return 0;
  }

  if (cli.mode == CLI_MODE_SELF_TEST) {
    app_workdir_init(argv[0]);
    setlocale(LC_ALL, "");
    return run_embedded_self_test();
  }

  if (cli.mode == CLI_MODE_RUN)
    return run_headless(&cli, argv[0]);

  app_workdir_init(argv[0]);

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
  long long last_idle_render_ms = main_now_ms();

  while (1) {
    int ch = getch();
    if (ch == ERR) {
      int had_http_events = http_poll(L);
      plugins_watch_poll();
      napms(10);
      long long now = main_now_ms();
      int active = http_is_loading() || agent_is_thinking() || had_http_events;
      long long interval =
          active ? ACTIVE_RENDER_INTERVAL_MS : IDLE_RENDER_INTERVAL_MS;
      if (now - last_idle_render_ms >= interval) {
        render_all();
        last_idle_render_ms = now;
      }
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
