#include "plugins.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>

lua_State *L = NULL;

void plugins_init(void) {
  L = luaL_newstate();
  luaL_openlibs(L);
}

static char *lua_getstr_f_value(lua_State *L, int idx, const char *field) {
  // Достаем из таблицы плагина поле id и после этого это
  // поле будет наверху стека (-1) а таблица будет уже (-2)
  // если бы искали поле в таблице с индексом -2 - значение полч потом все равно
  // бы оказалось на вершине стека
  lua_getfield(L, idx, field);

  const char *strval = lua_tostring(L, idx);
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
  // специальном реестре возвращая при этом ссылку на нее
  p->handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_pop(L, 1); // Это уже саму таблицу плагина удаляем со стека

  p->callback = NULL;
  p->user_data = NULL;

  return p;
}

const char *plugin_execute_sync(Plugin *plugin, const char *command,
                                char **args, int argc) {}
void plugins_cleanup(void) {
  if (L) {
    lua_close(L);
    L = NULL;
  }
}
