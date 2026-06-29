#include "plugins.h"
#include "plugins_internal.h"
#include "embedded_assets.h"
#include "log.h"
#include "popup.h"
#include "utils.h"
#include <dirent.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define INITIAL_PLUGIN_CAPACITY 8

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int plugins_file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

char *plugins_read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }

  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);

  char *buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t n = fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (n != (size_t)size) {
    free(buf);
    return NULL;
  }

  buf[n] = '\0';
  if (out_size)
    *out_size = n;
  return buf;
}

int plugins_lua_dobuffer_named(lua_State *l, const char *name,
                               const char *data, size_t size) {
  if (luaL_loadbuffer(l, data, size, name) != LUA_OK)
    return LUA_ERRSYNTAX;
  return lua_pcall(l, 0, LUA_MULTRET, 0);
}

static PluginRegistry plugins_registry = {
    .plugins = NULL, .count = 0, .capacity = 0};

static char g_watch_dir[512] = "";
static time_t g_last_plugin_scan = 0;

static void remove_plugin_hooks(const char *id) {
  if (!id || !L)
    return;
  char source[256];
  snprintf(source, sizeof(source), "plugin:%s", id);
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, "hooks");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return;
  }
  lua_getfield(L, -1, "remove_source");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 3);
    return;
  }
  lua_pushstring(L, source);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    fprintf(stderr, "Error removing plugin hooks: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

static void plugin_free(Plugin *p) {
  if (!p)
    return;
  remove_plugin_hooks(p->id);
  free(p->id);
  free(p->name);
  free(p->description);
  free(p->command);
  free(p->source_path);
  if (p->handler_ref > 0)
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->handler_ref);
  free(p->autocomplete_title);
  if (p->fetch_ref > 0)
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->fetch_ref);
  free(p->tool_name);
  free(p->tool_desc);
  free(p->tool_params_json);
  free(p->tool_permission);
  free(p);
}

static void remove_lua_plugin_entry(const char *id) {
  if (!id || !L)
    return;
  lua_getglobal(L, "plugins");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_pushnil(L);
  lua_setfield(L, -2, id);
  lua_pop(L, 1);
}

char *get_plugins_info() {
  size_t needed = snprintf(NULL, 0, "count: %d", plugins_registry.count) + 1;
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];
    if (!p || !p->command)
      continue;
    needed += snprintf(NULL, 0, "*** %s[%s] *** ",
                       p->name ? p->name : "", p->command);
  }

  char *result = malloc(needed);
  if (!result)
    return NULL;

  size_t len = snprintf(result, needed, "count: %d", plugins_registry.count);
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];
    if (!p || !p->command)
      continue;
    len += snprintf(result + len, needed - len, "*** %s[%s] *** ",
                    p->name ? p->name : "", p->command);
  }

  return result;
}

void plugin_registry_add(Plugin *plugin) {
  if (!plugin)
    return;
  if (plugins_registry.count >= plugins_registry.capacity) {
    int new_capacity =
        plugins_registry.capacity ? plugins_registry.capacity * 2
                                  : INITIAL_PLUGIN_CAPACITY;
    Plugin **new_plugins =
        realloc(plugins_registry.plugins, new_capacity * sizeof(Plugin *));
    if (!new_plugins) {
      plugin_free(plugin);
      return;
    }
    plugins_registry.plugins = new_plugins;
    plugins_registry.capacity = new_capacity;
  }
  plugins_registry.plugins[plugins_registry.count++] = plugin;
}

Plugin *plugin_registry_find(const char *command) {
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];

    if (!p || !p->command)
      continue;

    if (strcmp(p->command, command) == 0)
      return p;
  }
  return NULL;
}

void plugin_registry_cleanup(void) {
  if (!plugins_registry.plugins)
    return;

  for (int i = 0; i < plugins_registry.count; i++) {
    plugin_free(plugins_registry.plugins[i]);
  }
  free(plugins_registry.plugins);
  plugins_registry.plugins = NULL;
  plugins_registry.count = 0;
  plugins_registry.capacity = 0;
}

int plugin_registry_count(void) { return plugins_registry.count; }

Plugin *plugin_registry_at(int index) {
  if (index < 0 || index >= plugins_registry.count)
    return NULL;
  return plugins_registry.plugins[index];
}

static char *lua_get_optional_string(lua_State *L, int idx, const char *field,
                                     const char *fallback) {
  lua_getfield(L, idx, field);

  const char *strval = lua_tostring(L, -1);
  char *result = strval ? my_strdup(strval)
                        : (fallback ? my_strdup(fallback) : NULL);
  lua_pop(L, 1);
  return result;
}

static char *lua_get_required_string(lua_State *L, int idx, const char *field) {
  return lua_get_optional_string(L, idx, field, NULL);
}

static void install_plugin_hooks(lua_State *L, int plugin_idx) {
  int abs_idx = lua_absindex(L, plugin_idx);
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, "hooks");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return;
  }
  lua_getfield(L, -1, "install_plugin");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 3);
    return;
  }
  lua_pushvalue(L, abs_idx);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    fprintf(stderr, "Error installing plugin hooks: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

static void install_plugin_hooks_by_id(const char *id) {
  if (!id)
    return;
  lua_getglobal(L, "plugins");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, id);
  if (lua_istable(L, -1))
    install_plugin_hooks(L, -1);
  lua_pop(L, 2);
}

static void record_plugin_error(const char *source, const char *message) {
  char log_msg[2048];
  snprintf(log_msg, sizeof(log_msg), "source=%s error=%s",
           source ? source : "unknown", message ? message : "unknown");
  log_event("plugin", log_msg);

  if (!L)
    return;
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "capstan");
  }

  lua_getfield(L, -1, "plugin_errors");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "plugin_errors");
  }

  lua_newtable(L);
  lua_pushstring(L, source ? source : "unknown");
  lua_setfield(L, -2, "source");
  lua_pushstring(L, message ? message : "unknown");
  lua_setfield(L, -2, "message");
  lua_rawseti(L, -2, (int)lua_rawlen(L, -2) + 1);
  lua_pop(L, 2);
}

static Plugin *plugin_load_from_chunk(const char *name, const char *data,
                                      size_t size) {
  if (plugins_lua_dobuffer_named(L, name, data, size) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "Error loading plugin: %s\n", err);
    record_plugin_error(name, err);
    lua_pop(L, 1);
    return NULL;
  }
  if (!lua_istable(L, -1)) {
    fprintf(stderr, "Plugin must return a table\n");
    record_plugin_error(name, "Plugin must return a table");
    lua_pop(L, 1);
    return NULL;
  }

  lua_pushstring(L, name ? name : "unknown");
  lua_setfield(L, -2, "_source_path");

  Plugin *p = calloc(1, sizeof(Plugin));
  if (!p) {
    record_plugin_error(name, "Out of memory allocating plugin");
    lua_pop(L, 1);
    return NULL;
  }

  p->L = L;

  lua_getfield(L, -1, "history");
  p->include_in_history = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : 1;
  lua_pop(L, 1);

  p->id = lua_get_required_string(L, -1, "id");
  if (!p->id) {
    fprintf(stderr, "Plugin must define id\n");
    record_plugin_error(name, "Plugin must define id");
    free(p);
    lua_pop(L, 1);
    return NULL;
  }
  p->name = lua_get_optional_string(L, -1, "name", p->id);
  p->description = lua_get_optional_string(L, -1, "description", "");
  p->command = lua_get_optional_string(L, -1, "command", NULL);
  lua_getfield(L, -1, "handler");
  if (lua_isfunction(L, -1)) {
    /* Store the handler in the Lua registry so it survives GC and can be
       retrieved later by integer reference. */
    p->handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    lua_pop(L, 1);
    p->handler_ref = LUA_NOREF;
  }

  lua_getfield(L, -1, "autocomplete");
  if (lua_istable(L, -1)) {
    p->has_autocomplete = 1;

    lua_getfield(L, -1, "limit");
    p->autocomplete_limit =
        lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 10;
    lua_pop(L, 1);

    lua_getfield(L, -1, "multi");
    p->autocomplete_multi = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : 1;
    lua_pop(L, 1);

    lua_getfield(L, -1, "title");
    if (lua_isstring(L, -1))
      p->autocomplete_title = my_strdup(lua_tostring(L, -1));
    else
      p->autocomplete_title = NULL;
    lua_pop(L, 1);

    lua_getfield(L, -1, "fetch");
    p->fetch_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    p->has_autocomplete = 0;
    p->fetch_ref = 0;
    p->autocomplete_title = NULL;
  }
  lua_pop(L, 1);

  lua_getfield(L, -1, "tool");
  if (lua_istable(L, -1)) {
    p->has_tool = 1;

    lua_getfield(L, -1, "name");
    p->tool_name = lua_isstring(L, -1)
                       ? my_strdup(lua_tostring(L, -1))
                       : my_strdup("");
    lua_pop(L, 1);

    lua_getfield(L, -1, "description");
    p->tool_desc = lua_isstring(L, -1)
                       ? my_strdup(lua_tostring(L, -1))
                       : my_strdup("");
    lua_pop(L, 1);

    lua_getfield(L, -1, "permission");
    p->tool_permission = lua_isstring(L, -1)
                             ? my_strdup(lua_tostring(L, -1))
                             : my_strdup(p->tool_name);
    lua_pop(L, 1);

    lua_getfield(L, -1, "parameters");
    if (lua_istable(L, -1)) {
      lua_getglobal(L, "require");
      lua_pushstring(L, "vendor.rxi.json");
      if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
        lua_getfield(L, -1, "encode");
        lua_pushvalue(L, -3);
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1))
          p->tool_params_json = my_strdup(lua_tostring(L, -1));
        else
          p->tool_params_json = my_strdup("{}");
        lua_pop(L, 2);
      } else {
        lua_pop(L, 1);
        p->tool_params_json = my_strdup("{}");
      }
    } else {
      p->tool_params_json = my_strdup("{}");
    }
    lua_pop(L, 1);
  } else {
    p->has_tool = 0;
    p->tool_name = NULL;
    p->tool_desc = NULL;
    p->tool_params_json = NULL;
    p->tool_permission = NULL;
  }
  lua_pop(L, 1);

  lua_getglobal(L, "plugins");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "plugins");
  }
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, p->id);
  lua_pop(L, 2);

  return p;
}

Plugin *plugin_load(const char *path) {
  size_t size = 0;
  char *data = plugins_read_file(path, &size);
  if (!data) {
    fprintf(stderr, "Error reading plugin: %s\n", path);
    return NULL;
  }
  Plugin *p = plugin_load_from_chunk(path, data, size);
  if (p) {
    struct stat st;
    p->source_path = my_strdup(path);
    p->is_user_plugin = 1;
    if (stat(path, &st) == 0) {
      p->source_mtime = (long)st.st_mtime;
      p->source_size = (long)st.st_size;
    }
  }
  free(data);
  return p;
}

static int l_ctx_replace(lua_State *l) {
  const char *ui_val = luaL_checkstring(l, 2);
  const char *llm_val = lua_isnoneornil(l, 3) ? ui_val : luaL_checkstring(l, 3);
  lua_pushstring(l, ui_val);
  lua_pushstring(l, llm_val);
  return 2;
}

static char *dup_lua_result(lua_State *l, int index, const char *fallback) {
  if (lua_isnoneornil(l, index))
    return my_strdup(fallback ? fallback : "");

  const char *s = lua_tostring(l, index);
  if (s)
    return my_strdup(s);

  int abs_index = lua_absindex(l, index);
  luaL_tolstring(l, abs_index, NULL);
  s = lua_tostring(l, -1);
  char *copy = my_strdup(s ? s : (fallback ? fallback : ""));
  lua_pop(l, 1);
  return copy;
}

PluginResult *plugin_execute(Plugin *plugin, const char *input, size_t cmd_end,
                             char **pre_args, int pre_arg_count) {
  if (!plugin || plugin->handler_ref == LUA_NOREF) {
    popup_show_message("Plugin Error", "Plugin has no handler", 1);
    return NULL;
  }

  lua_newtable(L); // ctx = {}

  lua_pushstring(L, input);
  lua_setfield(L, -2, "input");

  lua_pushstring(L, plugin->command);
  lua_setfield(L, -2, "command");

  // ctx.args = {}
  lua_newtable(L);
  int arg_idx = 1;

  if (pre_args && pre_arg_count > 0) {
    for (int i = 0; i < pre_arg_count; i++) {
      lua_pushstring(L, pre_args[i]);
      lua_rawseti(L, -2, arg_idx++);
    }
  } else {
    const char *args_start = input + cmd_end;
    while (*args_start == ' ')
      args_start++;

    const char *p = args_start;
    while (*p) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;

      const char *word_start;
      size_t word_len;

      if (*p == '"') {
        p++;
        word_start = p;
        while (*p && *p != '"')
          p++;
        word_len = p - word_start;
        if (*p == '"')
          p++;
      } else {
        word_start = p;
        while (*p && *p != ' ' && *p != '"')
          p++;
        word_len = p - word_start;
      }

      lua_pushlstring(L, word_start, word_len);
      lua_rawseti(L, -2, arg_idx++);
    }
  }
  lua_setfield(L, -2, "args");

  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");

  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");
  lua_remove(L, -2);
  lua_insert(L, -2);
  lua_rawgeti(L, LUA_REGISTRYINDEX, plugin->handler_ref);
  lua_insert(L, -2);

  if (lua_pcall(L, 1, 2, 1) != LUA_OK) {
    popup_show_message("Plugin Error", lua_tostring(L, -1), 1);
    record_plugin_error(plugin->source_path ? plugin->source_path : plugin->id,
                        lua_tostring(L, -1));
    lua_pop(L, 2);
    return NULL;
  }

  PluginResult *r = malloc(sizeof(PluginResult));
  if (!r) {
    lua_pop(L, 3);
    return NULL;
  }

  r->ui_result = dup_lua_result(L, -2, "");
  r->raw_result = dup_lua_result(L, -1, r->ui_result ? r->ui_result : "");
  if (!r->ui_result || !r->raw_result) {
    free(r->ui_result);
    free(r->raw_result);
    free(r);
    lua_pop(L, 3);
    return NULL;
  }
  lua_pop(L, 3);

  return r;
}

int plugin_has_autocomplete(Plugin *plugin) {
  return plugin->has_autocomplete;
}

static char *lua_popup_item_str(lua_State *L, int idx, const char *field) {
  lua_getfield(L, idx, field);
  const char *s = lua_tostring(L, -1);
  char *result = s ? my_strdup(s) : my_strdup("");
  lua_pop(L, 1);
  return result;
}

void plugin_autocomplete_fetch(Plugin *plugin, const char *input, size_t cmd_end,
                               PopupItem **out_items, int *out_count,
                               char **out_title, int *out_limit,
                               int *out_multi) {
  *out_items = NULL;
  *out_count = 0;
  *out_title = NULL;
  *out_limit = plugin->autocomplete_limit;
  *out_multi = plugin->autocomplete_multi;

  lua_rawgeti(L, LUA_REGISTRYINDEX, plugin->fetch_ref);

  // Parse partial args for fetch(args)
  const char *args_start = input + cmd_end;
  while (*args_start == ' ')
    args_start++;
  lua_newtable(L);
  int arg_idx = 1;
  const char *p = args_start;
  while (*p) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    const char *word_start;
    size_t word_len;
    if (*p == '"') {
      p++;
      word_start = p;
      while (*p && *p != '"')
        p++;
      word_len = p - word_start;
      if (*p == '"')
        p++;
    } else {
      word_start = p;
      while (*p && *p != ' ' && *p != '"')
        p++;
      word_len = p - word_start;
    }
    lua_pushlstring(L, word_start, word_len);
    lua_rawseti(L, -2, arg_idx++);
  }

  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    fprintf(stderr, "autocomplete fetch: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return;
  }

  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  int count = (int)lua_rawlen(L, -1);
  if (count == 0) {
    lua_pop(L, 1);
    return;
  }

  *out_items = malloc(count * sizeof(PopupItem));
  for (int i = 0; i < count; i++) {
    lua_rawgeti(L, -1, i + 1);
    if (lua_istable(L, -1)) {
      (*out_items)[i].text = lua_popup_item_str(L, -1, "text");
      (*out_items)[i].value = lua_popup_item_str(L, -1, "value");
    } else if (lua_isstring(L, -1)) {
      const char *s = lua_tostring(L, -1);
      (*out_items)[i].text = my_strdup(s);
      (*out_items)[i].value = my_strdup(s);
    } else {
      (*out_items)[i].text = my_strdup("");
      (*out_items)[i].value = my_strdup("");
    }
    lua_pop(L, 1);
  }
  *out_count = count;

  if (plugin->autocomplete_title)
    *out_title = my_strdup(plugin->autocomplete_title);
  else if (plugin->name)
    *out_title = my_strdup(plugin->name);
  else
    *out_title = my_strdup("");

  lua_pop(L, 1);
}

void plugin_registry_remove_by_id(const char *id) {
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];
    if (p && strcmp(p->id, id) == 0) {
      plugin_free(p);
      memmove(&plugins_registry.plugins[i],
              &plugins_registry.plugins[i + 1],
              (plugins_registry.count - i - 1) * sizeof(Plugin *));
      plugins_registry.count--;
      return;
    }
  }
}

static void plugin_registry_remove_by_source_keep_id(const char *source_path,
                                                     const char *keep_id) {
  if (!source_path)
    return;
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];
    if (p && p->source_path && strcmp(p->source_path, source_path) == 0) {
      if (!keep_id || strcmp(p->id, keep_id) != 0)
        remove_lua_plugin_entry(p->id);
      plugin_free(p);
      memmove(&plugins_registry.plugins[i],
              &plugins_registry.plugins[i + 1],
              (plugins_registry.count - i - 1) * sizeof(Plugin *));
      plugins_registry.count--;
      return;
    }
  }
}

static int is_lua_file(const char *name) {
  size_t len = name ? strlen(name) : 0;
  return len > 4 && strcmp(name + len - 4, ".lua") == 0;
}

static Plugin *plugin_registry_find_by_source(const char *source_path) {
  if (!source_path)
    return NULL;
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];
    if (p && p->source_path && strcmp(p->source_path, source_path) == 0)
      return p;
  }
  return NULL;
}

static void plugin_registry_clear_for_reload(void) {
  for (int i = 0; i < plugins_registry.count; i++)
    plugin_free(plugins_registry.plugins[i]);
  plugins_registry.count = 0;

  lua_newtable(L);
  lua_setglobal(L, "plugins");
}

void load_plugins_from(const char *dir_path) {
  DIR *dir = opendir(dir_path);
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!is_lua_file(entry->d_name))
      continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
    Plugin *p = plugin_load(path);
    if (p) {
      plugin_registry_remove_by_source_keep_id(path, p->id);
      plugin_registry_remove_by_id(p->id);
      install_plugin_hooks_by_id(p->id);
      plugin_registry_add(p);
    }
  }
  closedir(dir);
}

static void reload_plugin_file(const char *path) {
  Plugin *p = plugin_load(path);
  if (!p)
    return;
  plugin_registry_remove_by_source_keep_id(path, p->id);
  plugin_registry_remove_by_id(p->id);
  install_plugin_hooks_by_id(p->id);
  plugin_registry_add(p);
  log_event("plugin", "reloaded user plugin");
}

static void rebuild_plugins_from_sources(void) {
  plugin_registry_clear_for_reload();
  load_embedded_plugins();
  if (g_watch_dir[0])
    load_plugins_from(g_watch_dir);
}

void plugins_watch_start(const char *dir_path) {
  if (!dir_path || !dir_path[0])
    return;
  snprintf(g_watch_dir, sizeof(g_watch_dir), "%s", dir_path);
  load_plugins_from(g_watch_dir);
  g_last_plugin_scan = time(NULL);
}

void plugins_watch_poll(void) {
  if (!g_watch_dir[0])
    return;

  time_t now = time(NULL);
  if (now == g_last_plugin_scan)
    return;
  g_last_plugin_scan = now;

  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];
    if (p && p->is_user_plugin && p->source_path) {
      struct stat st;
      if (stat(p->source_path, &st) != 0) {
        rebuild_plugins_from_sources();
        return;
      }
    }
  }

  DIR *dir = opendir(g_watch_dir);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!is_lua_file(entry->d_name))
      continue;
    char path[PATH_MAX];
    int written =
        snprintf(path, sizeof(path), "%s/%s", g_watch_dir, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path))
      continue;
    struct stat st;
    if (stat(path, &st) != 0)
      continue;
    Plugin *existing = plugin_registry_find_by_source(path);
    if (!existing || existing->source_mtime != (long)st.st_mtime ||
        existing->source_size != (long)st.st_size) {
      reload_plugin_file(path);
    }
  }
  closedir(dir);
}

int plugins_mcp_tick(void) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  lua_getfield(L, -1, "mcp");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return 0;
  }
  lua_getfield(L, -1, "tick");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 3);
    return 0;
  }
  lua_pushinteger(L, 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    record_plugin_error("agent.mcp", lua_tostring(L, -1));
    lua_pop(L, 3);
    return 0;
  }
  int changed = lua_toboolean(L, -1);
  lua_pop(L, 3);
  return changed;
}

void load_embedded_plugins(void) {
  size_t count = 0;
  const EmbeddedAsset *assets = embedded_assets(&count);
  for (size_t i = 0; i < count; i++) {
    const char *path = assets[i].path;
    if (strncmp(path, "plugins/", 8) != 0)
      continue;
    size_t len = strlen(path);
    if (len < 4 || strcmp(path + len - 4, ".lua") != 0)
      continue;

    Plugin *p = plugin_load_from_chunk(path, assets[i].data, assets[i].size);
    if (p) {
      p->source_path = my_strdup(path);
      p->is_user_plugin = 0;
      plugin_registry_remove_by_id(p->id);
      install_plugin_hooks_by_id(p->id);
      plugin_registry_add(p);
    }
  }
}
