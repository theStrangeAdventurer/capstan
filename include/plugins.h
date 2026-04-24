#ifndef PLUGINS_H
#define PLUGINS_H

#include <lua.h>

typedef struct Plugin Plugin;
typedef void (*PluginCallback)(Plugin *plugin, const char *result,
                               void *user_data);
struct Plugin {
  // Метаданные
  char *id;
  char *name;
  char *description;
  char *command;
  int is_async;

  // Обработчик
  lua_State *L;
  int handler_ref;

  // Для асинхронных операций
  int async_id;            // ID асинхронной операции
  int is_processing;       // Флаг выполнения
  PluginCallback callback; // Коллбек для результата
  void *user_data;         // Пользовательские данные
};

void plugins_init(void);

Plugin *plugin_load(const char *path);

const char *plugin_execute_sync(Plugin *plugin, const char *command,
                                char **args, int argc);

void plugins_cleanup();

#endif
