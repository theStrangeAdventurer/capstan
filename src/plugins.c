#include "plugins.h"
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

      free(p);
    }
  }
  free(plugins_registry.plugins);
  plugins_registry.plugins = NULL;
  plugins_registry.count = 0;
  plugins_registry.capacity = 0;
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

  lua_pop(L, 1); // Это уже саму таблицу плагина удаляем со стека

  p->callback = NULL;
  p->user_data = NULL;
  return p;
}

char *plugin_execute_sync(Plugin *plugin, const char *input) {
  // Достаем функицю по ссылке-индексу из специальной таблицы lua на стороне C и
  // кладем на вершину стека
  lua_rawgeti(plugin->L, LUA_REGISTRYINDEX, plugin->handler_ref);

  // Пушим команду на вершину стека
  // Тут функия уезжает на -2
  lua_pushstring(plugin->L, input);

  // Вызываем функцию - 1 аргумента, 1 результат, 0 ошибок
  // Стек выглядит так:
  // [-2] "input"                  первый аргумент
  // [-3] функция                    функция
  // lua_pcall берет со стека указанное количество аргументов и вызывает функцию
  // под ними
  // После чего кладет результат выполнения функции обратно на стек
  if (lua_pcall(plugin->L, 1, 2, 0) != LUA_OK) {
    fprintf(stderr, "Error: %s\n", lua_tostring(plugin->L, -1));
    return NULL;
  }
  // Теперь на стеке:
  // [-2] ui_result (первый возврат из Lua)
  // [-1] llm_result (второй возврат или nil)
  const char *ui_result = lua_tostring(plugin->L, -2);
  const char *llm_result = NULL;

  if (!lua_isnil(plugin->L, -1)) {
    llm_result = lua_tostring(plugin->L, -1);
  }

  if (!llm_result) {
    llm_result = ui_result; // TODO: подумать что с этим делать, видимо надо
                            // заводить структуру с двумя полями
  }

  static char response_buffer[PLUGIN_RESPONSE_MAX_SIZE];

  strncpy(response_buffer, ui_result, sizeof(response_buffer));
  lua_pop(plugin->L, 1);

  return response_buffer;
}
void plugins_cleanup(void) {
  if (L) {
    lua_close(L);
    L = NULL;
  }
}
