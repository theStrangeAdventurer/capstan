#include "acp.h"
#include "agent.h"
#include "app_config.h"
#include "cli_args.h"
#include "clipboard.h"
#include "dispatch.h"
#include "http.h"
#include "input.h"
#include "input_history.h"
#include "jsonl.h"
#include "linemap.h"
#include "log.h"
#include "mode.h"
#include "plugins.h"
#include "popup.h"
#include "scroll.h"
#include "session.h"
#include "session_manager.h"
#include "tui.h"
#include "trace.h"
#include "utils.h"
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
  int model_requests;
  int tool_calls;
  int turns;
  long long duration_ms;
  int duration_present;
  long long model_ms;
  long long tool_ms;
  long long permission_wait_ms;
  long long subagent_wait_ms;
  long long agent_started_ms;
  long long agent_finished_ms;
  long long session_save_ms;
  char error[1024];
} HeadlessRun;

static HeadlessRun g_headless_run = {0};
static TraceWriter g_headless_trace = TRACE_WRITER_INIT;
static int g_mouse_selecting_messages = 0;

#define ACTIVE_RENDER_INTERVAL_MS 16
#define IDLE_RENDER_INTERVAL_MS 50
#define KEY_CTRL_D 0x04

static long long main_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void terminal_reset_mouse_modes(void) {
  fputs("\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l\033[?2004l",
        stdout);
  fflush(stdout);
}

static void terminal_enable_bracketed_paste(void) {
  fputs("\033[?2004h", stdout);
  fflush(stdout);
}

static void terminal_disable_bracketed_paste(void) {
  fputs("\033[?2004l", stdout);
  fflush(stdout);
}

static int getch_wait_ms(int *out_ch, int timeout_ms) {
  for (int elapsed = 0; elapsed <= timeout_ms; elapsed++) {
    int ch = getch();
    if (ch != ERR) {
      if (out_ch)
        *out_ch = ch;
      return 1;
    }
    napms(1);
  }
  return 0;
}

static void unget_chars(const int *chars, int count) {
  for (int i = count - 1; i >= 0; i--)
    ungetch(chars[i]);
}

static int read_exact_after_escape(const char *suffix) {
  int consumed[16];
  int count = 0;
  for (size_t i = 0; suffix[i]; i++) {
    int ch;
    if (!getch_wait_ms(&ch, 20)) {
      unget_chars(consumed, count);
      return 0;
    }
    if (count < (int)(sizeof(consumed) / sizeof(consumed[0])))
      consumed[count++] = ch;
    if (ch != (unsigned char)suffix[i]) {
      unget_chars(consumed, count);
      return 0;
    }
  }
  return 1;
}

static int read_backspace_after_escape(void) {
  int ch;
  if (!getch_wait_ms(&ch, 20))
    return 0;
  if (ch == 127 || ch == 8 || ch == KEY_BACKSPACE)
    return 1;
  ungetch(ch);
  return 0;
}

static int read_bracketed_paste(void) {
  int matched = 0;

  while (1) {
    int ch;
    if (!getch_wait_ms(&ch, 100))
      return 1;

    if (matched == 0) {
      if (ch == APP_KEY_ESCAPE) {
        matched = 1;
        continue;
      }
      if (ch == '\r')
        ch = '\n';
      input_insert(ch);
      continue;
    }

    const char end_marker[] = "[201~";
    if (ch == (unsigned char)end_marker[matched - 1]) {
      matched++;
      if (end_marker[matched - 1] == '\0')
        return 1;
      continue;
    }

    input_insert(APP_KEY_ESCAPE);
    for (int i = 0; i < matched - 1; i++)
      input_insert(end_marker[i]);
    if (ch == '\r')
      ch = '\n';
    input_insert(ch);
    matched = 0;
  }
}

static int message_pane_geometry(int *out_y, int *out_x, int *out_h,
                                 int *out_w) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  int badge_h =
      (g_buffered_results.size > 0 && !popup_is_active() &&
       !popup_is_message_active())
          ? 1
          : 0;
  int queue_h = (!popup_is_active() && !popup_is_message_active())
                    ? dispatch_queue_visible_size()
                    : 0;
  int msg_h = rows - INPUT_WIN_HEIGHT - 2 * MARGIN - badge_h - queue_h;
  int inner_w = cols - 2 * MARGIN;
  if (msg_h < 1 || inner_w < 1)
    return 0;

  if (out_y)
    *out_y = MARGIN;
  if (out_x)
    *out_x = MARGIN;
  if (out_h)
    *out_h = msg_h;
  if (out_w)
    *out_w = inner_w;
  return 1;
}

static int message_half_page_lines(void) {
  int pane_h = 0;
  if (!message_pane_geometry(NULL, NULL, &pane_h, NULL))
    return 1;
  int half_page = pane_h / 2;
  return half_page > 0 ? half_page : 1;
}

static int mouse_to_visual_position(const MEVENT *event, int *out_line,
                                    int *out_col) {
  int pane_y, pane_x, pane_h, pane_w;
  if (!message_pane_geometry(&pane_y, &pane_x, &pane_h, &pane_w))
    return 0;
  if (event->y < pane_y || event->y >= pane_y + pane_h || event->x < pane_x ||
      event->x >= pane_x + pane_w)
    return 0;

  int total_lines = linemap_count();
  if (total_lines <= 0)
    return 0;

  int top_line = total_lines - pane_h - scroll_get();
  if (top_line < 0)
    top_line = 0;

  int line = top_line + (event->y - pane_y);
  if (line < 0)
    line = 0;
  if (line >= total_lines)
    line = total_lines - 1;

  int col = event->x - pane_x - MSG_PAD_H;
  if (col < 0)
    col = 0;

  if (out_line)
    *out_line = line;
  if (out_col)
    *out_col = col;
  return 1;
}

static void focus_messages_at(int line, int col) {
  mode_set(FOCUS_MESSAGES);
  if (!visual_cursor_visible())
    visual_enter();
  visual_set_cursor(line, col);
}

static void toggle_focus(void) {
  if (mode_get() == FOCUS_MESSAGES)
    visual_exit();
  mode_toggle();
  if (mode_get() == FOCUS_MESSAGES)
    visual_resume();
}

static void copy_active_selection(void) {
  Messages *msgs = get_messages();
  if (!msgs || msgs->size == 0)
    return;

  const char **texts = malloc(msgs->size * sizeof(const char *));
  if (!texts)
    return;
  for (size_t i = 0; i < msgs->size; i++)
    texts[i] = msgs->items[i]->text;
  visual_yank(texts, (int)msgs->size);
  free(texts);
}

static void flash_copy_active_selection(void) {
  if (!visual_is_active())
    return;

  int sl, sc, el, ec;
  visual_selection_range(&sl, &sc, &el, &ec);
  if (sl == el && sc == ec)
    return;

  render_all();
  napms(70);
  visual_exit_selection();
  render_all();
  napms(70);
  visual_enter_selection_at(sl, sc);
  visual_set_cursor(el, ec);
  render_all();
  napms(70);

  copy_active_selection();
  popup_show_message_ms("Copied", "Text copied", 0, 500);
}

static int stop_active_stream(void) {
  if (!http_is_loading())
    return 0;
  int compacting = strcmp(agent_activity(), "Compacting") == 0;
  if (http_cancel_streams(L) <= 0)
    return 0;
  agent_set_thinking(0);
  agent_set_activity(NULL);
  agent_finish_run();
  if (!compacting)
    append_to_last_message("\n[stopped]\n", MSG_AGENT);
  return 1;
}

static void handle_mouse_event(MEVENT *event) {
  if (event->bstate & BUTTON4_PRESSED) {
    scroll_up(3);
    return;
  }
  if (event->bstate & BUTTON5_PRESSED) {
    scroll_down(3);
    return;
  }

  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  if ((event->bstate &
       (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED)) &&
      tui_focus_input_at_point(rows, cols, event->y, event->x)) {
    g_mouse_selecting_messages = 0;
    return;
  }

  int line, col;
  if (event->bstate & BUTTON1_CLICKED) {
    if (mouse_to_visual_position(event, &line, &col)) {
      g_mouse_selecting_messages = 0;
      focus_messages_at(line, col);
      visual_exit_selection();
    }
    return;
  }

  if (event->bstate & BUTTON1_PRESSED) {
    if (mouse_to_visual_position(event, &line, &col)) {
      g_mouse_selecting_messages = 1;
      mode_set(FOCUS_MESSAGES);
      if (!visual_cursor_visible())
        visual_enter();
      visual_enter_selection_at(line, col);
    }
    return;
  }

  if (event->bstate & BUTTON1_RELEASED) {
    if (g_mouse_selecting_messages &&
        mouse_to_visual_position(event, &line, &col)) {
      visual_set_cursor(line, col);
      flash_copy_active_selection();
    }
    g_mouse_selecting_messages = 0;
    return;
  }
}

static void print_help(void) {
  printf("Usage:\n");
  printf("  capstan [options]\n");
  printf("  capstan run [--prompt TEXT | --prompt-file PATH] [options]\n");
  printf("  capstan acp [--yolo]\n\n");
  printf("Options:\n");
  printf("  --provider NAME     Override provider for this run\n");
  printf("  --model ID          Override model for this run\n");
  printf("  --profile NAME      Configured agent profile\n");
  printf("  --reasoning-effort LEVEL\n");
  printf("                      Reasoning effort: none, minimal, low, medium, high, xhigh, max\n");
  printf("  --workdir PATH      Override command and relative-file directory\n");
  printf("  --workspace PATH    Override workspace root and permission boundary\n");
  printf("  --session-id ID     Resume or create a workspace session\n");
  printf("  --max-turns N       Limit agent continuation rounds (default: 200)\n");
  printf("  --no-mcp           Do not start configured MCP servers for this run\n");
  printf("  --no-wiki          Do not load or initialize wiki context for this run\n");
  printf("  --no-preserve-reasoning\n");
  printf("                      Do not return prior reasoning with tool results\n");
  printf("  --yolo             Auto-allow tool calls except explicit denies\n");
  printf("  --benchmark        Isolated workspace-scoped eval mode\n");
  printf("  --json              Print structured JSON result\n");
  printf("  --trace-file PATH   Write safe structured JSONL run trace\n");
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

static int l_headless_on_model_start(lua_State *l) {
  int turn = (int)luaL_optinteger(l, 1, 0);
  int attempt = (int)luaL_optinteger(l, 2, 0);
  int messages = (int)luaL_optinteger(l, 3, 0);
  int tools = (int)luaL_optinteger(l, 4, 0);
  int prompt_tokens = (int)luaL_optinteger(l, 5, 0);
  const char *provider = luaL_optstring(l, 6, "");
  const char *model = luaL_optstring(l, 7, "");
  const char *effort = luaL_optstring(l, 8, "");
  const char *profile = luaL_optstring(l, 9, "");
  int preserve_reasoning = lua_toboolean(l, 10);
  size_t body_bytes = (size_t)luaL_optinteger(l, 11, 0);
  const char *purpose = luaL_optstring(l, 12, "agent");

  JsonlBuffer data;
  jsonl_buffer_init(&data);
  jsonl_append_format(
      &data,
      "{\"turn\":%d,\"attempt\":%d,\"messages\":%d,"
      "\"tools\":%d,\"prompt_tokens_estimate\":%d,\"provider\":",
      turn, attempt, messages, tools, prompt_tokens);
  jsonl_append_string(&data, provider);
  jsonl_append(&data, ",\"model\":");
  jsonl_append_string(&data, model);
  jsonl_append(&data, ",\"reasoning_effort\":");
  if (effort[0])
    jsonl_append_string(&data, effort);
  else
    jsonl_append(&data, "null");
  jsonl_append(&data, ",\"profile\":");
  if (profile[0])
    jsonl_append_string(&data, profile);
  else
    jsonl_append(&data, "null");
  jsonl_append_format(&data,
                      ",\"preserve_reasoning\":%s,\"body_bytes\":%zu,"
                      "\"purpose\":",
                      preserve_reasoning ? "true" : "false", body_bytes);
  jsonl_append_string(&data, purpose);
  jsonl_append(&data, "}");
  int written = !data.failed &&
                trace_event(&g_headless_trace, "model.request.started",
                            data.data);
  jsonl_buffer_free(&data);
  if (!written)
    return luaL_error(l, "failed to write model.request.started trace event");
  return 0;
}

static void jsonl_append_lua_integer(JsonlBuffer *json, lua_State *l,
                                     int index, const char *field) {
  if (!lua_istable(l, index)) {
    jsonl_append(json, "null");
    return;
  }
  lua_getfield(l, index, field);
  if (lua_isnumber(l, -1))
    jsonl_append_format(json, "%lld", (long long)lua_tointeger(l, -1));
  else
    jsonl_append(json, "null");
  lua_pop(l, 1);
}

static void jsonl_append_lua_object_integer(JsonlBuffer *json, lua_State *l,
                                            int index, const char *object,
                                            const char *field) {
  if (!lua_istable(l, index)) {
    jsonl_append(json, "null");
    return;
  }
  lua_getfield(l, index, object);
  jsonl_append_lua_integer(json, l, -1, field);
  lua_pop(l, 1);
}

static void jsonl_append_lua_string(JsonlBuffer *json, lua_State *l,
                                    int index, const char *field) {
  if (!lua_istable(l, index)) {
    jsonl_append(json, "null");
    return;
  }
  lua_getfield(l, index, field);
  if (lua_isstring(l, -1))
    jsonl_append_string(json, lua_tostring(l, -1));
  else
    jsonl_append(json, "null");
  lua_pop(l, 1);
}

static int l_headless_on_model_done(lua_State *l) {
  int turn = (int)luaL_optinteger(l, 1, 0);
  int attempt = (int)luaL_optinteger(l, 2, 0);
  int ok = lua_toboolean(l, 3);
  int duration = (int)luaL_optinteger(l, 4, 0);
  int text_bytes = (int)luaL_optinteger(l, 5, 0);
  int reasoning_bytes = (int)luaL_optinteger(l, 6, 0);
  int tool_calls = (int)luaL_optinteger(l, 7, 0);
  g_headless_run.model_requests++;
  g_headless_run.model_ms += duration;

  JsonlBuffer data;
  jsonl_buffer_init(&data);
  jsonl_append_format(&data,
      "{\"turn\":%d,\"attempt\":%d,\"ok\":%s,\"duration_ms\":%d,"
      "\"request_outcome\":", turn, attempt, ok ? "true" : "false", duration);
  jsonl_append_lua_string(&data, l, 8, "request_outcome");
  jsonl_append(&data, ",\"first_output_ms\":");
  jsonl_append_lua_integer(&data, l, 8, "first_output_ms");
  jsonl_append(&data, ",\"first_reasoning_ms\":");
  jsonl_append_lua_integer(&data, l, 8, "first_reasoning_ms");
  jsonl_append(&data, ",\"first_text_ms\":");
  jsonl_append_lua_integer(&data, l, 8, "first_text_ms");
  jsonl_append(&data, ",\"first_tool_ms\":");
  jsonl_append_lua_integer(&data, l, 8, "first_tool_ms");
  jsonl_append_format(&data,
      ",\"text_bytes\":%d,\"reasoning_bytes\":%d,\"tool_calls\":%d,"
      "\"usage\":{\"prompt_tokens\":", text_bytes, reasoning_bytes, tool_calls);
  jsonl_append_lua_object_integer(&data, l, 8, "usage", "prompt_tokens");
  jsonl_append(&data, ",\"completion_tokens\":");
  jsonl_append_lua_object_integer(&data, l, 8, "usage", "completion_tokens");
  jsonl_append(&data, ",\"reasoning_tokens\":");
  jsonl_append_lua_object_integer(&data, l, 8, "usage", "reasoning_tokens");
  jsonl_append(&data, "},\"transport\":{");
  const char *fields[][2] = {
      {"http_status", "http_status"}, {"curl_code", "curl_code"},
      {"namelookup_elapsed_ms", "namelookup_elapsed_ms"},
      {"connect_elapsed_ms", "connect_elapsed_ms"},
      {"appconnect_elapsed_ms", "appconnect_elapsed_ms"},
      {"pretransfer_elapsed_ms", "pretransfer_elapsed_ms"},
      {"starttransfer_elapsed_ms", "starttransfer_elapsed_ms"},
      {"dns_ms", "dns_ms"}, {"tcp_connect_ms", "tcp_connect_ms"},
      {"tls_handshake_ms", "tls_handshake_ms"},
      {"request_setup_ms", "request_setup_ms"},
      {"upload_and_server_wait_ms", "upload_and_server_wait_ms"},
      {"download_ms", "download_ms"},
      {"ttfb_ms", "ttfb_ms"}, {"total_ms", "total_ms"},
      {"uploaded_bytes", "uploaded_bytes"},
      {"downloaded_bytes", "downloaded_bytes"},
      {"chunk_count", "chunk_count"}, {"redirect_count", "redirect_count"},
  };
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    if (i) jsonl_append(&data, ",");
    jsonl_append_string(&data, fields[i][0]);
    jsonl_append(&data, ":");
    jsonl_append_lua_object_integer(&data, l, 8, "transport", fields[i][1]);
  }
  jsonl_append(&data, "}}");
  int written = !data.failed &&
                trace_event(&g_headless_trace, "model.request.finished",
                            data.data);
  jsonl_buffer_free(&data);
  if (!written)
    return luaL_error(l, "failed to write model.request.finished trace event");
  return 0;
}

static int l_headless_on_tool_start(lua_State *l) {
  lua_getfield(l, 1, "name");
  const char *name = luaL_optstring(l, -1, "unknown");
  int written = trace_tool_event(&g_headless_trace, "tool.started", name,
                                 0, 0, 0, 0, 0);
  lua_pop(l, 1);
  if (!written)
    return luaL_error(l, "failed to write tool.started trace event");
  return 0;
}

static int l_headless_on_tool_done(lua_State *l) {
  lua_getfield(l, 1, "name");
  const char *name = luaL_optstring(l, -1, "unknown");
  size_t result_size = 0;
  (void)luaL_optlstring(l, 2, "", &result_size);
  int ok = lua_toboolean(l, 3);
  int duration = (int)luaL_optinteger(l, 4, 0);
  int permission_wait = (int)luaL_optinteger(l, 5, 0);
  if (permission_wait < 0) permission_wait = 0;
  if (permission_wait > duration) permission_wait = duration;
  g_headless_run.tool_calls++;
  trace_accumulate_tool_breakdown(
      strcmp(name, "subagents") == 0, duration, permission_wait,
      &g_headless_run.tool_ms, &g_headless_run.permission_wait_ms,
      &g_headless_run.subagent_wait_ms);
  int written = trace_tool_event(&g_headless_trace, "tool.finished", name,
                                 1, ok, duration, permission_wait,
                                 result_size);
  lua_pop(l, 1);
  if (!written)
    return luaL_error(l, "failed to write tool.finished trace event");
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

    lua_getfield(l, 1, "turns");
    g_headless_run.turns = (int)luaL_optinteger(l, -1, 0);
    lua_pop(l, 1);
    lua_getfield(l, 1, "duration_ms");
    if (lua_isinteger(l, -1)) {
      lua_Integer duration = lua_tointeger(l, -1);
      if (duration >= 0) {
        g_headless_run.duration_ms = (long long)duration;
        g_headless_run.duration_present = 1;
      }
    }
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

static int headless_session_begin(const CliOptions *opts, const char *prompt,
                                  Session *session, char *error,
                                  size_t error_size) {
  memset(session, 0, sizeof(*session));
  const char *id = opts->session_id;
  int created = 0;
  if (!id)
    return 1;
  if (!session_id_valid(id)) {
    snprintf(error, error_size,
             "invalid session ID: use a non-empty name under %d bytes "
             "without slashes, control characters, or edge spaces",
             SESSION_ID_SIZE);
    return 0;
  }
  if (!session_store_init(app_workspace_root())) {
    snprintf(error, error_size, "could not initialize session storage");
    return 0;
  }
  if (!session_load_or_create_named(session, id, &created)) {
    snprintf(error, error_size,
             "session '%s' could not be loaded or created", id);
    return 0;
  }

  SessionMessage *grown =
      realloc(session->messages,
              (session->message_count + 1) * sizeof(SessionMessage));
  if (!grown) {
    if (created)
      session_delete(session->id);
    session_free(session);
    snprintf(error, error_size, "could not allocate session history");
    return 0;
  }
  session->messages = grown;
  SessionMessage *message = &session->messages[session->message_count];
  memset(message, 0, sizeof(*message));
  message->role = SESSION_ROLE_USER;
  message->text = my_strdup(prompt);
  message->raw_text = my_strdup(prompt);
  if (!message->text || !message->raw_text) {
    free(message->text);
    free(message->raw_text);
    memset(message, 0, sizeof(*message));
    if (created)
      session_delete(session->id);
    session_free(session);
    snprintf(error, error_size, "could not allocate session history");
    return 0;
  }
  session->message_count++;
  session->updated_at = time(NULL);
  if (!session_save(session)) {
    if (created)
      session_delete(session->id);
    session_free(session);
    snprintf(error, error_size, "could not save session '%s'", id);
    return 0;
  }
  if (!log_set_session_id(session->id)) {
    if (created)
      session_delete(session->id);
    session_free(session);
    snprintf(error, error_size, "could not initialize session log scope");
    return 0;
  }
  return 1;
}

static void lua_push_session_message(lua_State *l,
                                     const SessionMessage *message) {
  lua_newtable(l);
  lua_pushstring(l, message->role == SESSION_ROLE_USER ? "user"
                                                       : "assistant");
  lua_setfield(l, -2, "role");

  const char *text = message->raw_text ? message->raw_text
                                       : (message->text ? message->text : "");
  if (message->role == SESSION_ROLE_USER && message->image_count > 0) {
    lua_newtable(l);
    lua_newtable(l);
    lua_pushstring(l, "text");
    lua_setfield(l, -2, "type");
    lua_pushstring(l, text);
    lua_setfield(l, -2, "text");
    lua_rawseti(l, -2, 1);
    for (size_t i = 0; i < message->image_count; i++) {
      const SessionImage *image = &message->images[i];
      lua_newtable(l);
      lua_pushstring(l, "image_url");
      lua_setfield(l, -2, "type");
      lua_newtable(l);
      lua_pushfstring(l, "data:%s;base64,%s", image->mime_type, image->data);
      lua_setfield(l, -2, "url");
      lua_setfield(l, -2, "image_url");
      lua_rawseti(l, -2, (lua_Integer)i + 2);
    }
  } else {
    lua_pushstring(l, text);
  }
  lua_setfield(l, -2, "content");
}

static void lua_push_headless_messages(lua_State *l, const Session *session,
                                       const char *prompt) {
  lua_newtable(l);
  if (session->id[0]) {
    for (size_t i = 0; i < session->message_count; i++) {
      lua_push_session_message(l, &session->messages[i]);
      lua_rawseti(l, -2, (lua_Integer)i + 1);
    }
    return;
  }

  lua_newtable(l);
  lua_pushstring(l, "user");
  lua_setfield(l, -2, "role");
  lua_pushstring(l, prompt);
  lua_setfield(l, -2, "content");
  lua_rawseti(l, -2, 1);
}

static int headless_session_finish(Session *session,
                                   const HeadlessRun *run) {
  if (!session->id[0])
    return 1;
  if (run->text && run->text[0]) {
    SessionMessage *grown =
        realloc(session->messages,
                (session->message_count + 1) * sizeof(SessionMessage));
    if (!grown)
      return 0;
    session->messages = grown;
    SessionMessage *message = &session->messages[session->message_count];
    memset(message, 0, sizeof(*message));
    message->role = SESSION_ROLE_ASSISTANT;
    message->text = my_strdup(run->text);
    message->raw_text = my_strdup(run->text);
    if (!message->text || !message->raw_text) {
      free(message->text);
      free(message->raw_text);
      memset(message, 0, sizeof(*message));
      return 0;
    }
    session->message_count++;
  }
  session->updated_at = time(NULL);
  int ok = session_save(session);
  log_event("session", ok ? "headless session finished"
                           : "headless session save failed");
  return ok;
}

static int finish_headless_trace(const CliOptions *opts, const char *stage) {
  if (!opts->trace_file)
    return 1;
  if (g_headless_trace.state == TRACE_STATE_PUBLISHED)
    return 1;

  long long now = trace_monotonic_ms();
  long long process_duration = now - g_headless_trace.started_monotonic_ms;
  if (process_duration < 0) process_duration = 0;
  long long agent_wall = 0;
  if (g_headless_run.agent_started_ms > 0) {
    long long finished = g_headless_run.agent_finished_ms > 0
                             ? g_headless_run.agent_finished_ms
                             : now;
    agent_wall = finished - g_headless_run.agent_started_ms;
    if (agent_wall < 0) agent_wall = 0;
  }
  long long agent_duration = g_headless_run.duration_present
                                 ? g_headless_run.duration_ms
                                 : agent_wall;
  long long startup = g_headless_run.agent_started_ms > 0
                          ? g_headless_run.agent_started_ms -
                                g_headless_trace.started_monotonic_ms
                          : process_duration;
  if (startup < 0) startup = 0;
  long long post_agent = process_duration - startup - agent_wall -
                         g_headless_run.session_save_ms;
  long long process_conservation_error = 0;
  if (post_agent < 0) {
    process_conservation_error = post_agent;
    post_agent = 0;
  }

  long long residual = agent_duration - g_headless_run.model_ms -
                       g_headless_run.tool_ms -
                       g_headless_run.permission_wait_ms -
                       g_headless_run.subagent_wait_ms;
  long long unattributed = residual >= 0 ? residual : 0;
  long long conservation_error = residual < 0 ? residual : 0;

  JsonlBuffer data;
  jsonl_buffer_init(&data);
  jsonl_append_format(
      &data,
      "{\"ok\":%s,\"intended_exit_code\":%d,\"failure_stage\":",
      g_headless_run.ok ? "true" : "false", g_headless_run.ok ? 0 : 1);
  jsonl_append_string(&data, stage ? stage : "");
  jsonl_append_format(
      &data,
      ",\"turns\":%d,\"duration_ms\":%lld,"
      "\"timing\":{\"process_duration_ms\":%lld,\"startup_ms\":%lld,"
      "\"agent_wall_ms\":%lld,\"agent_runtime_ms\":%lld,"
      "\"session_save_ms\":%lld,\"post_agent_ms\":%lld,"
      "\"conservation_error_ms\":%lld},"
      "\"counts\":{\"model_requests\":%d,\"tool_calls\":%d},"
      "\"breakdown\":{\"model_ms\":%lld,\"tool_ms\":%lld,"
      "\"permission_wait_ms\":%lld,\"subagent_wait_ms\":%lld,"
      "\"unattributed_ms\":%lld,\"conservation_error_ms\":%lld}}",
      g_headless_run.turns, process_duration, process_duration, startup,
      agent_wall, agent_duration, g_headless_run.session_save_ms, post_agent,
      process_conservation_error, g_headless_run.model_requests,
      g_headless_run.tool_calls, g_headless_run.model_ms,
      g_headless_run.tool_ms, g_headless_run.permission_wait_ms,
      g_headless_run.subagent_wait_ms, unattributed, conservation_error);

  int terminal_ok = !data.failed &&
                    trace_terminal_event(&g_headless_trace, "run.finished",
                                         data.data);
  jsonl_buffer_free(&data);
  char error[256] = "";
  if (!terminal_ok ||
      !trace_finish(&g_headless_trace, error, sizeof(error))) {
    if (!error[0])
      snprintf(error, sizeof(error), "requested trace terminal event failed");
    snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s", error);
    g_headless_run.ok = 0;
    return 0;
  }
  return 1;
}

static int fail_headless_trace(const CliOptions *opts, const char *message,
                               const char *stage) {
  g_headless_run.ok = 0;
  snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s",
           message ? message : "startup failed");
  return finish_headless_trace(opts, stage);
}

static void report_headless_error(const CliOptions *opts) {
  const char *message = g_headless_run.error[0] ? g_headless_run.error
                                                : "run failed";
  if (opts->json)
    print_json_result(0, "", message);
  else
    fprintf(stderr, "capstan: %s\n", message);
}

static int run_headless(const CliOptions *opts, const char *argv0) {
  g_headless_run = (HeadlessRun){0};
  g_headless_trace = (TraceWriter)TRACE_WRITER_INIT;
  if (opts->trace_file) {
    char trace_error[512];
    if (!trace_open(&g_headless_trace, opts->trace_file, trace_error,
                    sizeof(trace_error))) {
      if (opts->json)
        print_json_result(0, "", trace_error);
      else
        fprintf(stderr, "capstan: %s\n", trace_error);
      return 1;
    }
    if (!trace_event(&g_headless_trace, "run.started",
                     opts->benchmark ? "{\"kind\":\"benchmark\"}"
                                     : "{\"kind\":\"headless\"}")) {
      char finish_error[256] = "";
      (void)trace_finish(&g_headless_trace, finish_error,
                         sizeof(finish_error));
      snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s",
               finish_error[0] ? finish_error
                               : "requested trace could not write run.started");
      report_headless_error(opts);
      return 1;
    }
  }

  char *owned_prompt = NULL;
  const char *prompt = opts->prompt;
  if (!prompt && opts->prompt_file) {
    owned_prompt = read_file_prompt(opts->prompt_file);
    if (!owned_prompt) {
      char message[512];
      snprintf(message, sizeof(message), "cannot read prompt file: %s",
               opts->prompt_file);
      (void)fail_headless_trace(opts, message, "prompt");
      report_headless_error(opts);
      return 1;
    }
    prompt = owned_prompt;
  } else if (!prompt && !isatty(STDIN_FILENO)) {
    owned_prompt = read_all(stdin);
    prompt = owned_prompt;
  }
  if (!prompt || !prompt[0]) {
    const char *message = "run requires --prompt, --prompt-file, or stdin";
    (void)fail_headless_trace(opts, message, "prompt");
    report_headless_error(opts);
    free(owned_prompt);
    return 1;
  }

  app_workdir_init(argv0);
  if (opts->workdir && !app_workdir_set(opts->workdir)) {
    char message[512];
    snprintf(message, sizeof(message), "invalid --workdir: %s", opts->workdir);
    (void)fail_headless_trace(opts, message, "workdir");
    report_headless_error(opts);
    free(owned_prompt);
    return 1;
  }
  if (opts->workspace && !app_workspace_set(opts->workspace)) {
    char message[512];
    snprintf(message, sizeof(message), "invalid --workspace: %s", opts->workspace);
    (void)fail_headless_trace(opts, message, "workspace");
    report_headless_error(opts);
    free(owned_prompt);
    return 1;
  }

  setlocale(LC_ALL, "");
  http_set_headless(1);
  PluginsInitOptions plugin_options = {.disable_mcp = opts->no_mcp,
                                       .disable_wiki = opts->no_wiki,
                                       .isolated = opts->benchmark};
  plugins_init_with_options(&plugin_options);

  Session headless_session = {0};
  char session_error[256] = "";
  if (!headless_session_begin(opts, prompt, &headless_session, session_error,
                              sizeof(session_error))) {
    (void)fail_headless_trace(opts, session_error, "session");
    report_headless_error(opts);
    plugins_cleanup();
    free(owned_prompt);
    return 1;
  }
  load_embedded_plugins();
  char global_plugins[512];
  if (!opts->benchmark &&
      app_config_path(global_plugins, sizeof(global_plugins), "plugins") == 0)
    load_plugins_from(global_plugins);
  if (headless_session.id[0])
    log_event("session", "headless session started");

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");

  lua_newtable(L);
  lua_push_headless_messages(L, &headless_session, prompt);
  lua_setfield(L, -2, "messages");
  if (opts->provider) {
    lua_pushstring(L, opts->provider);
    lua_setfield(L, -2, "provider");
  }
  if (opts->model) {
    lua_pushstring(L, opts->model);
    lua_setfield(L, -2, "model");
  }
  if (opts->profile) {
    lua_pushstring(L, opts->profile);
    lua_setfield(L, -2, "profile");
  }
  if (opts->reasoning_effort) {
    lua_pushstring(L, opts->reasoning_effort);
    lua_setfield(L, -2, "reasoning_effort");
  }
  if (opts->no_preserve_reasoning) {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "preserve_reasoning");
  }
  lua_pushinteger(L, opts->max_turns);
  lua_setfield(L, -2, "max_turns");
  if (opts->yolo || opts->benchmark) {
    lua_newtable(L);
    lua_pushboolean(L, opts->benchmark);
    lua_setfield(L, -2, "full_control");
    lua_pushboolean(L, opts->yolo);
    lua_setfield(L, -2, "yolo");
    lua_pushboolean(L, opts->benchmark);
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
  lua_pushcfunction(L, l_headless_on_model_start);
  lua_setfield(L, -2, "on_model_start");
  lua_pushcfunction(L, l_headless_on_model_done);
  lua_setfield(L, -2, "on_model_done");
  lua_pushcfunction(L, l_headless_on_tool_start);
  lua_setfield(L, -2, "on_tool_start");
  lua_pushcfunction(L, l_headless_on_tool_done);
  lua_setfield(L, -2, "on_tool_done");

  g_headless_run.agent_started_ms = trace_monotonic_ms();
  if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
    const char *message = lua_tostring(L, -1);
    snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s",
             message ? message : "run failed");
    lua_pop(L, 1);
    g_headless_run.agent_finished_ms = trace_monotonic_ms();
    g_headless_run.ok = 0;
    (void)finish_headless_trace(opts, "start");
    report_headless_error(opts);
    plugins_cleanup();
    session_free(&headless_session);
    free(owned_prompt);
    return 1;
  }
  int started = lua_toboolean(L, -2);
  const char *start_err = lua_tostring(L, -1);
  char start_error[1024];
  snprintf(start_error, sizeof(start_error), "%s",
           start_err ? start_err : "run failed");
  lua_pop(L, 4);
  if (!started) {
    snprintf(g_headless_run.error, sizeof(g_headless_run.error), "%s",
             start_error);
    g_headless_run.agent_finished_ms = trace_monotonic_ms();
    g_headless_run.ok = 0;
    (void)finish_headless_trace(opts, "dispatch");
    report_headless_error(opts);
    plugins_cleanup();
    session_free(&headless_session);
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
  g_headless_run.agent_finished_ms = trace_monotonic_ms();

  long long session_started_ms = trace_monotonic_ms();
  int session_saved = headless_session_finish(&headless_session,
                                               &g_headless_run);
  g_headless_run.session_save_ms = trace_monotonic_ms() - session_started_ms;
  if (!session_saved && g_headless_run.ok) {
    snprintf(g_headless_run.error, sizeof(g_headless_run.error),
             "agent completed but session could not be saved");
    g_headless_run.ok = 0;
  }

  (void)finish_headless_trace(opts, g_headless_run.ok ? "" : "completion");

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
  session_free(&headless_session);
  plugins_cleanup();
  free(owned_prompt);
  return rc;
}

static int run_embedded_self_test(void) {
  plugins_init();
  load_embedded_plugins();

  const char *expected[] = {"/file", "/write", "/edit", "/shell", "/fetch",
                            "/logs", "/skills", "/models", "/info", "/mcp",
                            "/plan", "/implement", "/auth",
                            "/logout", "/connect", "/vcs"};
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
    lua_getfield(L, -1, "agent");
    if (!lua_istable(L, -1)) {
      printf("missing capstan.agent\n");
      ok = 0;
    } else {
      lua_getfield(L, -1, "run");
      if (!lua_isfunction(L, -1)) {
        printf("missing capstan.agent.run\n");
        ok = 0;
      }
      lua_pop(L, 1);

      lua_getfield(L, -1, "set_profile");
      if (!lua_isfunction(L, -1)) {
        printf("missing capstan.agent.set_profile\n");
        ok = 0;
      }
      lua_pop(L, 1);

      lua_getfield(L, -1, "profiles");
      if (!lua_isfunction(L, -1)) {
        printf("missing capstan.agent.profiles\n");
        ok = 0;
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);

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

static int lua_agent_get_profile(lua_State *l, char *out, size_t out_size) {
  if (!out || out_size == 0)
    return 0;
  out[0] = '\0';

  int top = lua_gettop(l);
  lua_getglobal(l, "capstan");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "agent");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "get_profile");
  if (!lua_isfunction(l, -1))
    goto done;
  if (lua_pcall(l, 0, 1, 0) != LUA_OK)
    goto done;
  const char *profile = lua_tostring(l, -1);
  if (!profile || !profile[0])
    goto done;
  snprintf(out, out_size, "%s", profile);
  lua_settop(l, top);
  return 1;

done:
  lua_settop(l, top);
  return 0;
}

static void lua_agent_set_profile(lua_State *l, const char *profile) {
  int top = lua_gettop(l);
  lua_getglobal(l, "capstan");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "agent");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "set_profile");
  if (!lua_isfunction(l, -1))
    goto done;
  lua_pushstring(l, profile);
  lua_pcall(l, 1, 2, 0);

done:
  lua_settop(l, top);
}

static void lua_agent_set_yolo(lua_State *l, int enabled) {
  int top = lua_gettop(l);
  lua_getglobal(l, "capstan");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "agent");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "set_yolo");
  if (!lua_isfunction(l, -1))
    goto done;
  lua_pushboolean(l, enabled);
  lua_pcall(l, 1, 0, 0);

done:
  lua_settop(l, top);
}

static int lua_agent_configure_interactive(lua_State *l,
                                           const CliOptions *opts,
                                           char *error, size_t error_size) {
  int top = lua_gettop(l);
  int ok = 0;
  const char *message = NULL;

  lua_getglobal(l, "capstan");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "agent");
  if (!lua_istable(l, -1))
    goto done;
  lua_getfield(l, -1, "configure_interactive");
  if (!lua_isfunction(l, -1))
    goto done;

  lua_newtable(l);
  if (opts->provider) {
    lua_pushstring(l, opts->provider);
    lua_setfield(l, -2, "provider");
  }
  if (opts->model) {
    lua_pushstring(l, opts->model);
    lua_setfield(l, -2, "model");
  }
  if (opts->profile) {
    lua_pushstring(l, opts->profile);
    lua_setfield(l, -2, "profile");
  }
  if (opts->reasoning_effort) {
    lua_pushstring(l, opts->reasoning_effort);
    lua_setfield(l, -2, "reasoning_effort");
  }
  if (opts->max_turns_set) {
    lua_pushinteger(l, opts->max_turns);
    lua_setfield(l, -2, "max_turns");
  }
  if (opts->no_preserve_reasoning) {
    lua_pushboolean(l, 0);
    lua_setfield(l, -2, "preserve_reasoning");
  }

  if (lua_pcall(l, 1, 2, 0) != LUA_OK) {
    message = lua_tostring(l, -1);
    goto done;
  }
  ok = lua_toboolean(l, -2);
  if (!ok)
    message = lua_tostring(l, -1);

done:
  if (!ok && error && error_size > 0)
    snprintf(error, error_size, "%s",
             message ? message : "interactive options are unavailable");
  lua_settop(l, top);
  return ok;
}

static void cycle_agent_profile(lua_State *l) {
  char current[64];
  if (!lua_agent_get_profile(l, current, sizeof(current))) {
    const char *published = agent_profile_name();
    snprintf(current, sizeof(current), "%s",
             published && published[0] ? published : "implement");
  }

  int top = lua_gettop(l);
  lua_getglobal(l, "capstan");
  lua_getfield(l, -1, "agent");
  lua_getfield(l, -1, "profiles");
  if (!lua_isfunction(l, -1) || lua_pcall(l, 0, 1, 0) != LUA_OK ||
      !lua_istable(l, -1)) {
    lua_settop(l, top);
    return;
  }
  size_t count = lua_rawlen(l, -1);
  size_t selected = 1;
  for (size_t i = 1; i <= count; i++) {
    lua_rawgeti(l, -1, (lua_Integer)i);
    const char *name = lua_tostring(l, -1);
    if (name && strcmp(name, current) == 0)
      selected = i == count ? 1 : i + 1;
    lua_pop(l, 1);
  }
  lua_rawgeti(l, -1, (lua_Integer)selected);
  const char *next = lua_tostring(l, -1);
  if (next)
    lua_agent_set_profile(l, next);
  lua_settop(l, top);
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

  if (cli.mode == CLI_MODE_ACP)
    return acp_run(argv[0], cli.yolo);

  app_workdir_init(argv[0]);
  if (cli.workdir && !app_workdir_set(cli.workdir)) {
    fprintf(stderr, "capstan: invalid --workdir: %s\n", cli.workdir);
    return 2;
  }
  if (cli.workspace && !app_workspace_set(cli.workspace)) {
    fprintf(stderr, "capstan: invalid --workspace: %s\n", cli.workspace);
    return 2;
  }

  setlocale(LC_ALL, "");
  if (!verify_terminal())
    return 1;

  set_escdelay(50);
  initscr();
  /* Deliver control keys immediately instead of waiting for a cooked line. */
  cbreak();
  noecho();
  timeout(0);
  keypad(stdscr, TRUE);
  terminal_reset_mouse_modes();
  mousemask(ALL_MOUSE_EVENTS, NULL);
  terminal_enable_bracketed_paste();
  init_tui();
  popup_init();

  PluginsInitOptions plugin_options = {.disable_mcp = cli.no_mcp,
                                       .disable_wiki = cli.no_wiki};
  plugins_init_with_options(&plugin_options);
  load_embedded_plugins();

  char cli_error[256] = "";
  if (!lua_agent_configure_interactive(L, &cli, cli_error,
                                       sizeof(cli_error))) {
    terminal_disable_bracketed_paste();
    terminal_reset_mouse_modes();
    endwin();
    plugins_cleanup();
    fprintf(stderr, "capstan: %s\n", cli_error);
    return 2;
  }
  if (cli.yolo)
    lua_agent_set_yolo(L, 1);

  char global_plugins[512];
  if (app_config_path(global_plugins, sizeof(global_plugins), "plugins") == 0)
    plugins_watch_start(global_plugins);

  input_init();
  input_history_load(app_workspace_root());
  if (!session_manager_init_selected(app_workspace_root(), cli.session_id)) {
    if (cli.session_id) {
      terminal_disable_bracketed_paste();
      terminal_reset_mouse_modes();
      endwin();
      plugins_cleanup();
      fprintf(stderr, "capstan: could not load or create session '%s'\n",
              cli.session_id);
      return 2;
    }
    popup_show_message("Sessions", "Sessions are unavailable", 1);
  }
  scroll_reset();
  render_all();
  long long last_idle_render_ms = main_now_ms();
  long long last_mcp_tick_ms = 0;

  while (1) {
    int ch = getch();
    if (ch == ERR) {
      int had_http_events = http_poll_limited(L, 2);
      dispatch_tick();
      session_manager_tick();
      long long now = main_now_ms();
      int had_mcp_events = 0;
      if (now - last_mcp_tick_ms >= 30) {
        had_mcp_events = plugins_mcp_tick();
        last_mcp_tick_ms = now;
      }
      plugins_watch_poll();
      napms(10);
      int active = http_is_loading() || agent_is_running() ||
                   agent_is_thinking() || had_http_events || had_mcp_events;
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
      if (getmouse(&event) == OK)
        handle_mouse_event(&event);
      render_all();
      continue;
    }

    if (mode_is_profile_cycle_key(ch)) {
      cycle_agent_profile(L);
      render_all();
      continue;
    }

    if (ch == APP_KEY_ESCAPE && read_exact_after_escape("\t")) {
      toggle_focus();
      render_all();
      continue;
    }

    if (mode_is_focus_toggle_key(ch)) {
      toggle_focus();
      render_all();
      continue;
    }

    if (ch == APP_KEY_ESCAPE && mode_get() == FOCUS_INPUT) {
      if (read_backspace_after_escape()) {
        input_delete_word_backward();
        render_all();
        continue;
      }
      if (read_exact_after_escape("[200~")) {
        read_bracketed_paste();
        render_all();
        continue;
      }
      stop_active_stream();
      render_all();
      continue;
    }

    if (ch == '\t') {
      if (mode_get() == FOCUS_INPUT)
        dispatch_tab();
      render_all();
      continue;
    }

    if (ch == '/') {
      if (mode_get() == FOCUS_INPUT) {
        const char *text = input_get_text();
        if (text[0] == '\0') {
          input_insert(ch);
          dispatch_tab();
          render_all();
          continue;
        }
      }
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
      } else if (ch == TUI_KEY_CTRL_U) {
        int half_page = message_half_page_lines();
        scroll_up(half_page);
        int vc_line;
        visual_get_cursor(&vc_line, NULL);
        visual_set_cursor_line(vc_line + half_page);
      } else if (ch == KEY_CTRL_D) {
        int half_page = message_half_page_lines();
        scroll_down(half_page);
        int vc_line;
        visual_get_cursor(&vc_line, NULL);
        visual_set_cursor_line(vc_line - half_page);
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
        else if (ch == 'y')
          flash_copy_active_selection();
        else if (ch == 27)
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

    if (tui_handle_input_shortcut(ch)) {
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

    if (ch == KEY_UP) {
      input_set_text(input_history_prev(input_get_text()));
      render_all();
      continue;
    }

    if (ch == KEY_DOWN) {
      input_set_text(input_history_next(input_get_text()));
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
  session_manager_shutdown();
  terminal_disable_bracketed_paste();
  terminal_reset_mouse_modes();
  endwin();
  plugin_registry_cleanup();
  system("reset");
  return 0;
}
