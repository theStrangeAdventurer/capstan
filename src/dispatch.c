#include "dispatch.h"
#include "agent.h"
#include "app_config.h"
#include "editor.h"
#include "finder.h"
#include "http.h"
#include "input.h"
#include "input_history.h"
#include "plugins.h"
#include "popup.h"
#include "scroll.h"
#include "session_manager.h"
#include "submission_queue.h"
#include "tui.h"
#include "utils.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void finder_add_lua_string_array(FinderIgnoreList *ignore,
                                        const char *field, int load_files) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return;
  }
  lua_getfield(L, -1, "finder");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 3);
    return;
  }
  lua_getfield(L, -1, field);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 4);
    return;
  }

  int len = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, -1, i);
    const char *value = lua_tostring(L, -1);
    if (value) {
      if (load_files)
        finder_ignore_load_file(ignore, app_workdir(), value);
      else
        finder_ignore_add(ignore, value);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 4);
}

static int open_file_finder(Plugin *plugin, size_t cmd_end) {
  FinderIgnoreList ignore;
  finder_ignore_init(&ignore);
  finder_ignore_load_file(&ignore, app_workdir(), ".gitignore");
  finder_add_lua_string_array(&ignore, "ignore_files", 1);
  finder_add_lua_string_array(&ignore, "ignore_patterns", 0);

  PopupItem *items = NULL;
  int count = 0;
  int ok = finder_collect_files(app_workdir(), &ignore, &items, &count);
  finder_ignore_free(&ignore);
  if (!ok)
    return 0;

  if (count > 0)
    popup_open_filterable_with_plugin(items, count, "Files", 10, 0, plugin,
                                      cmd_end);
  popup_items_free(items, count);
  return count > 0;
}

static SubmissionQueue g_submission_queue = {0};
static int g_session_popup = 0;
static int g_dispatch_after_compact = 0;

static int open_sessions(void) {
  SessionInfo *sessions = NULL;
  size_t count = 0;
  if (!session_manager_list(&sessions, &count)) {
    popup_show_message("Sessions", "Could not read saved sessions", 1);
    return 0;
  }
  if (count == 0) {
    session_list_free(sessions);
    popup_show_message("Sessions", "No saved sessions", 0);
    return 1;
  }
  PopupItem *items = calloc(count, sizeof(PopupItem));
  if (!items) {
    session_list_free(sessions);
    return 0;
  }
  const char *active = session_manager_active_id();
  for (size_t i = 0; i < count; i++) {
    char label[256];
    char stamp[32] = "";
    struct tm *tm = localtime(&sessions[i].updated_at);
    if (tm)
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", tm);
    snprintf(label, sizeof(label), "%s %s  %s",
             strcmp(active, sessions[i].id) == 0 ? "*" : " ",
             sessions[i].title, stamp);
    items[i].text = my_strdup(label);
    items[i].value = my_strdup(sessions[i].id);
  }
  popup_open_filterable_with_plugin(items, (int)count, "Sessions", 10, 0,
                                    NULL, 0);
  g_session_popup = 1;
  popup_items_free(items, (int)count);
  session_list_free(sessions);
  return 1;
}

int dispatch_queue_size(void) {
  return submission_queue_size(&g_submission_queue);
}

int dispatch_queue_visible_size(void) {
  return submission_queue_visible_size(&g_submission_queue);
}

const char *dispatch_queue_at(int index) {
  return submission_queue_at(&g_submission_queue, index);
}

void dispatch_queue_clear(void) {
  submission_queue_clear(&g_submission_queue);
  g_dispatch_after_compact = 0;
}

static void flush_buffered_results(void) {
  if (g_buffered_results.size > 0) {
    for (int i = 0; i < g_buffered_results.size; i++) {
      add_message(g_buffered_results.items[i].ui_result,
                  g_buffered_results.items[i].raw_result, MSG_USER);
      g_buffered_results.items[i].ui_result = NULL;
      g_buffered_results.items[i].raw_result = NULL;
    }
    buffered_results_clear();
  }
}

static void flush_buffered_and_send(const char *ui_text, const char *raw_text,
                                    InputImage *images, size_t image_count) {
  flush_buffered_results();
  if (ui_text[0] || image_count > 0) {
    char *ui = my_strdup(ui_text);
    char *raw = my_strdup(raw_text);
    Messages *messages = get_messages();
    size_t previous_size = messages ? messages->size : 0;
    add_message(ui, raw, MSG_USER);
    Message *message = messages && messages->size > previous_size
                           ? messages->items[messages->size - 1]
                           : NULL;
    for (size_t i = 0; message && i < image_count; i++)
      message_add_image(message, images[i].mime_type, images[i].data);
  }
  input_images_free(images, image_count);
  char *empty = my_strdup("");
  add_message(empty, empty, MSG_AGENT);
  agent_build_and_dispatch(L);
}

static void add_error_and_emit(const char *error) {
  if (g_buffered_results.size > 0) {
    for (int i = 0; i < g_buffered_results.size; i++) {
      add_message(g_buffered_results.items[i].ui_result,
                  g_buffered_results.items[i].raw_result, MSG_USER);
      g_buffered_results.items[i].ui_result = NULL;
      g_buffered_results.items[i].raw_result = NULL;
    }
    buffered_results_clear();
  }
  char *cp_err = my_strdup(error);
  add_message(cp_err, cp_err, MSG_USER);
  char *empty = my_strdup("");
  add_message(empty, empty, MSG_AGENT);
  agent_build_and_dispatch(L);
}

static void add_plugin_result(Plugin *p, PluginResult *r) {
  const char *cmd = p->command;
  if (cmd[0] == '/')
    cmd++;
  char label[64];
  snprintf(label, sizeof(label), "ctx:%s", cmd);
  buffer_plugin_result(label, r->ui_result, r->raw_result);
  free(r);
}

static void show_plugin_result(Plugin *p, PluginResult *r) {
  popup_show_message(p->name ? p->name : "Command",
                     r->ui_result ? r->ui_result : "", 0);
  free(r->ui_result);
  free(r->raw_result);
  free(r);
}

static int try_builtin_or_plugin_command(const char *input, size_t cmd_end) {
  char command[MAX_COMMAND_LEN];
  size_t ce;
  if (!has_command(input, command, &ce))
    return 0;

  if (strcmp(command, "/editor") == 0) {
    const char *prompt = input + cmd_end;
    if (*prompt == ' ')
      prompt++;
    editor_open_prompt(prompt);
    return 2;
  }

  if (strcmp(command, "/new") == 0) {
    http_cancel_streams(L);
    agent_set_thinking(0);
    agent_finish_run();
    agent_reset_usage();
    buffered_results_clear();
    dispatch_queue_clear();
    if (!session_manager_new())
      popup_show_message("Sessions", "Could not create a new session", 1);
    scroll_reset();
    return 1;
  }

  if (strcmp(command, "/sessions") == 0) {
    open_sessions();
    return 1;
  }

  if (strcmp(command, "/compact") == 0) {
    agent_compact(L);
    return 1;
  }

  Plugin *p = plugin_registry_find(command);
  if (p) {
    if (plugin_has_autocomplete(p)) {
      PopupItem *items;
      int count;
      char *title;
      int limit, multi;
      plugin_autocomplete_fetch(p, input, cmd_end, &items, &count,
                                &title, &limit, &multi);
      if (count > 0) {
        if (strcmp(command, "/models") == 0)
          popup_open_filterable_with_plugin(items, count, title, limit, multi,
                                            p, cmd_end);
        else
          popup_open_with_plugin(items, count, title, limit, multi, p, cmd_end);
        free(title);
        for (int i = 0; i < count; i++) {
          free(items[i].text);
          free(items[i].value);
        }
        free(items);
        return 1;
      }
      free(title);
      return 1;
    }
    PluginResult *r = plugin_execute(p, input, cmd_end, NULL, 0);
    if (r) {
      if (p->include_in_history)
        add_plugin_result(p, r);
      else
        show_plugin_result(p, r);
    }
    return 1;
  }

  if (strcmp(command, "/") == 0) {
    int pc = plugin_registry_count();
    int builtins = 4;
    int command_plugins = 0;
    for (int i = 0; i < pc; i++) {
      Plugin *pp = plugin_registry_at(i);
      if (pp && pp->command)
        command_plugins++;
    }
    int count = command_plugins + builtins;
    if (count > 0) {
      PopupItem *items = malloc(count * sizeof(PopupItem));
      items[0].text = my_strdup("/editor  Edit prompt in $EDITOR");
      items[0].value = my_strdup("/editor");
      items[1].text = my_strdup("/new  Start a new session");
      items[1].value = my_strdup("/new");
      items[2].text = my_strdup("/sessions  Open saved sessions");
      items[2].value = my_strdup("/sessions");
      items[3].text = my_strdup("/compact  Compact conversation context");
      items[3].value = my_strdup("/compact");
      int out = builtins;
      for (int i = 0; i < pc; i++) {
        Plugin *pp = plugin_registry_at(i);
        if (!pp || !pp->command)
          continue;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s  %s", pp->command,
                 pp->description ? pp->description : "");
        items[out].text = my_strdup(buf);
        items[out].value = my_strdup(pp->command);
        out++;
      }
      popup_open_with_plugin(items, count, "Commands", 10, 0, NULL, 0);
      for (int i = 0; i < count; i++) {
        free(items[i].text);
        free(items[i].value);
      }
      free(items);
      return 1;
    }
  }

  char err[256];
  snprintf(err, sizeof(err), "Unknown command: %s", command);
  add_error_and_emit(err);
  return 1;
}

int dispatch_tab(void) {
  const char *text = input_get_text();
  char command[MAX_COMMAND_LEN];
  size_t cmd_end;

  if (!text[0] || !has_command(text, command, &cmd_end))
    return 0;

  if (strcmp(command, "/") == 0) {
    int pc = plugin_registry_count();
    int builtins = 4;
    int command_plugins = 0;
    for (int i = 0; i < pc; i++) {
      Plugin *pp = plugin_registry_at(i);
      if (pp && pp->command)
        command_plugins++;
    }
    int count = command_plugins + builtins;
    if (count <= 0)
      return 0;

    PopupItem *items = malloc(count * sizeof(PopupItem));
    items[0].text = my_strdup("/editor  Edit prompt in $EDITOR");
    items[0].value = my_strdup("/editor");
    items[1].text = my_strdup("/new  Start a new session");
    items[1].value = my_strdup("/new");
    items[2].text = my_strdup("/sessions  Open saved sessions");
    items[2].value = my_strdup("/sessions");
    items[3].text = my_strdup("/compact  Compact conversation context");
    items[3].value = my_strdup("/compact");
    int out = builtins;
    for (int i = 0; i < pc; i++) {
      Plugin *pp = plugin_registry_at(i);
      if (!pp || !pp->command)
        continue;
      char buf[256];
      snprintf(buf, sizeof(buf), "%s  %s", pp->command,
               pp->description ? pp->description : "");
      items[out].text = my_strdup(buf);
      items[out].value = my_strdup(pp->command);
      out++;
    }
    popup_open_with_plugin(items, count, "Commands", 10, 0, NULL, 0);
    for (int i = 0; i < count; i++) {
      free(items[i].text);
      free(items[i].value);
    }
    free(items);
    return 1;
  }

  Plugin *p = plugin_registry_find(command);
  if (!p || !plugin_has_autocomplete(p))
    return 0;

  if (strcmp(command, "/file") == 0)
    return open_file_finder(p, cmd_end);

  PopupItem *items;
  int count;
  char *title;
  int limit, multi;
  plugin_autocomplete_fetch(p, text, cmd_end, &items, &count,
                            &title, &limit, &multi);
  if (count > 0) {
    if (strcmp(command, "/models") == 0)
      popup_open_filterable_with_plugin(items, count, title, limit, multi, p,
                                        cmd_end);
    else
      popup_open_with_plugin(items, count, title, limit, multi, p, cmd_end);
  }
  for (int i = 0; i < count; i++) {
    free(items[i].text);
    free(items[i].value);
  }
  free(items);
  free(title);
  return count > 0;
}

void dispatch_tick(void) {
  if (agent_is_running() ||
      (submission_queue_size(&g_submission_queue) == 0 &&
       !g_dispatch_after_compact))
    return;

  g_dispatch_after_compact = 0;
  while (submission_queue_size(&g_submission_queue) > 0) {
    char *text = submission_queue_shift(&g_submission_queue);
    if (text)
      add_message(text, text, MSG_USER);
  }
  char *empty = my_strdup("");
  add_message(empty, empty, MSG_AGENT);
  scroll_reset();
  agent_build_and_dispatch(L);
}

void dispatch_submit(void) {
  const char *text = input_get_text();
  size_t image_count = input_image_count();
  if (!text[0] && image_count == 0 && g_buffered_results.size == 0)
    return;

  scroll_reset();
  char submitted[INPUT_BUFFER_SIZE];
  char display[INPUT_DISPLAY_BUFFER_SIZE];
  snprintf(submitted, sizeof(submitted), "%s", text);
  snprintf(display, sizeof(display), "%s", input_get_display_text());

  char command[MAX_COMMAND_LEN];
  size_t cmd_end;
  int is_command = image_count == 0 && submitted[0] &&
                   has_command(submitted, command, &cmd_end);

  if (agent_is_running() || submission_queue_size(&g_submission_queue) > 0) {
    if (image_count > 0) {
      popup_show_message_ms("Queued input",
                            "Image prompts cannot be queued while the agent is running",
                            0, 1600);
      return;
    }
    if (is_command) {
      popup_show_message_ms("Queued input",
                            "Commands are unavailable while the agent is running",
                            0, 1200);
      return;
    }
    if (!submitted[0])
      return;
    if (!submission_queue_push(&g_submission_queue, submitted)) {
      popup_show_message_ms("Queued input", "Queue is full (5 messages)", 0,
                            1200);
      return;
    }
    input_history_add(submitted);
    input_clear();
    render_all();
    return;
  }

  if (submitted[0])
    input_history_add(submitted);
  InputImage *images = input_take_images(&image_count);
  input_clear();
  render_all();

  if (is_command) {
    input_images_free(images, image_count);
    int handled = try_builtin_or_plugin_command(submitted, cmd_end);
    if (handled == 2)
      return;
  } else {
    flush_buffered_results();
    if (image_count == 0 && agent_should_auto_compact(L, submitted)) {
      if (submitted[0] &&
          !submission_queue_push(&g_submission_queue, submitted)) {
        flush_buffered_and_send(display, submitted, images, image_count);
        return;
      }
      input_images_free(images, image_count);
      g_dispatch_after_compact = 1;
      agent_auto_compact(L);
      return;
    }
    flush_buffered_and_send(display, submitted, images, image_count);
  }
}

void dispatch_popup_result(void) {
  char **selected;
  int sel_count;
  selected = popup_get_selected(&sel_count);
  if (selected && sel_count > 0) {
    Plugin *p = popup_get_plugin();
    size_t cmd_end = popup_get_cmd_end();
    if (!p) {
      if (g_session_popup) {
        if (!session_manager_switch(selected[0]))
          popup_show_message("Sessions", "Could not open the selected session",
                             1);
        else {
          buffered_results_clear();
          dispatch_queue_clear();
          input_clear();
          agent_reset_usage();
          scroll_reset();
        }
        g_session_popup = 0;
        popup_free_selected(selected, sel_count);
      } else {
        char buf[INPUT_BUFFER_SIZE];
        snprintf(buf, INPUT_BUFFER_SIZE, "%s ", selected[0]);
        input_set_text(buf);
        popup_free_selected(selected, sel_count);
      }
    } else if (plugin_has_autocomplete(p)) {
      int is_dir = 0;
      const char *dir_path = NULL;
      for (int i = 0; i < sel_count; i++) {
        size_t len = strlen(selected[i]);
        if (len > 0 && selected[i][len - 1] == '/') {
          is_dir = 1;
          dir_path = selected[i];
          break;
        }
      }
      if (is_dir && dir_path) {
        char fake_input[INPUT_BUFFER_SIZE];
        snprintf(fake_input, sizeof(fake_input), "%s %s", p->command, dir_path);
        size_t fake_cmd_end = strlen(p->command);
        PopupItem *items;
        int count;
        char *title;
        int limit, multi;
        plugin_autocomplete_fetch(p, fake_input, fake_cmd_end,
                                  &items, &count, &title, &limit, &multi);
        if (count > 0) {
          popup_drill_down(items, count, title);
          for (int i = 0; i < count; i++) {
            free(items[i].text);
            free(items[i].value);
          }
          free(items);
        }
        free(title);
        popup_free_selected(selected, sel_count);
        return;
      }
      PluginResult *r =
          plugin_execute(p, input_get_text(), cmd_end, selected, sel_count);
      if (r) {
        if (p->include_in_history)
          add_plugin_result(p, r);
        else
          show_plugin_result(p, r);
      }
      input_clear();
      popup_free_selected(selected, sel_count);
    } else {
      PluginResult *r =
          plugin_execute(p, input_get_text(), cmd_end, selected, sel_count);
      if (r) {
        if (p->include_in_history)
          add_plugin_result(p, r);
        else
          show_plugin_result(p, r);
      }
      input_clear();
      popup_free_selected(selected, sel_count);
    }
  }
  if (!selected || sel_count == 0)
    g_session_popup = 0;
  popup_close();
}
