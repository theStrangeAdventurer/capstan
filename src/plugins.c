#include "plugins.h"
#include "agent.h"
#include "http.h"
#include "utils.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define PLUGIN_RESPONSE_MAX_SIZE 4096
#define PLUGIN_CAPACITY_INCREMENT 10

lua_State *L = NULL;

void plugins_init(void) {
  L = luaL_newstate();
  luaL_openlibs(L);
  http_init(L);
  agent_init(L);
  if (luaL_dofile(L, "ai/providers.lua") != LUA_OK) {
    fprintf(stderr, "providers: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

static PluginRegistry plugins_registry = {
    .plugins = NULL, .count = 0, .capacity = 0};

char *get_plugins_info() {
  char *result = malloc(100 * plugins_registry.count);
  int len = 0;

  len += sprintf(result, "count: %d", plugins_registry.count);
  for (int i = 0; i < plugins_registry.count; i++) {
    Plugin *p = plugins_registry.plugins[i];

    if (!p || !p->command)
      continue;

    len += sprintf(result + len, "*** %s[%s] *** ", p->name, p->command);
  }

  return result;
}

void plugin_registry_add(Plugin *plugin) {
  if (plugins_registry.count >= plugins_registry.capacity) {
    plugins_registry.capacity += PLUGIN_CAPACITY_INCREMENT;
    plugins_registry.plugins =
        realloc(plugins_registry.plugins,
                plugins_registry.capacity *
                    sizeof(Plugin *)); // Выделяем место под большее количество
                                       // указателей на плагины
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
    Plugin *p = plugins_registry.plugins[i];

    if (p) {
      free(p->command);
      free(p->user_data);
      free(p->id);
      free(p->name);
      free(p->description);

      if (p->handler_ref > 0)
        luaL_unref(p->L, LUA_REGISTRYINDEX, p->handler_ref);

      free(p->autocomplete_title);
      if (p->fetch_ref > 0)
        luaL_unref(p->L, LUA_REGISTRYINDEX, p->fetch_ref);

      free(p);
    }
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

static char *lua_getstr_f_value(lua_State *L, int idx, const char *field) {
  // Достаем из таблицы плагина поле id и после этого это
  // поле будет наверху стека (-1) а таблица будет уже (-2)
  // если бы искали поле в таблице с индексом -2 - значение полч потом все равно
  // бы оказалось на вершине стека
  lua_getfield(L, idx, field);

  const char *strval = lua_tostring(L, -1); // Теперь берем с вершины стека
  char *result = malloc(strlen(strval) + 1);
  strcpy(result, strval);
  lua_pop(L, 1); // Снимаем  id со стека, теперь снова таблица на -1
  return result;
}

Plugin *plugin_load(const char *path) {
  if (luaL_dofile(L, path) !=
      LUA_OK) { // Выполняет lua файл и кладет результат выполнения на стек lua
    fprintf(stderr, "Error loading plugin: %s\n", lua_tostring(L, -1));
    return NULL;
  }
  if (!lua_istable(L, -1)) { // Проверяем самый верхний элемент на стеке
    fprintf(stderr, "Plugin must return a table\n");
    return NULL;
  }

  Plugin *p = malloc(sizeof(Plugin));

  p->L = L;

  lua_getfield(L, -1, "is_async");
  p->is_async = lua_toboolean(L, -1);
  lua_pop(L, 1);

  p->id = lua_getstr_f_value(L, -1, "id");
  p->name = lua_getstr_f_value(L, -1, "name");
  p->description = lua_getstr_f_value(L, -1, "description");
  p->command = lua_getstr_f_value(L, -1, "command");
  lua_getfield(L, -1, "handler");
  // Автоматически снимает с вершины стека функцию handler и сохраняет ее в
  // специальном реестре возвращая при этом ссылку (int) на нее
  // позже мы получим функцию по этой ссылке числу с помощью lua_rawgeti
  // LUA_REGISTRYINDEX - специальная таблица в которой можно безопасно хранить
  // данные lua чтобы их не собрал сборщик мусора
  p->handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);

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

  lua_pop(L, 1); // Это уже саму таблицу плагина удаляем со стека

  p->callback = NULL;
  p->user_data = NULL;
  return p;
}

static int l_ctx_replace(lua_State *l) {
  const char *ui_val = luaL_checkstring(l, 2);
  const char *llm_val = lua_isnoneornil(l, 3) ? ui_val : luaL_checkstring(l, 3);
  lua_pushstring(l, ui_val);
  lua_pushstring(l, llm_val);
  return 2;
}

PluginResult *plugin_execute(Plugin *plugin, const char *input, size_t cmd_end,
                             char **pre_args, int pre_arg_count) {
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

  lua_rawgeti(L, LUA_REGISTRYINDEX, plugin->handler_ref);
  lua_insert(L, -2);

  if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
    fprintf(stderr, "Plugin error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return NULL;
  }

  const char *ui_result = lua_tostring(L, -2);
  const char *raw_result = NULL;

  if (!lua_isnil(L, -1)) {
    raw_result = lua_tostring(L, -1);
  }

  if (!raw_result) {
    raw_result = ui_result;
  }

  PluginResult *r = malloc(sizeof(PluginResult));
  r->ui_result = my_strdup(ui_result);
  r->raw_result = my_strdup(raw_result ? raw_result : ui_result);
  lua_pop(L, 2);

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

void plugins_cleanup(void) {
  if (L) {
    lua_close(L);
    L = NULL;
  }
  http_cleanup();
}
