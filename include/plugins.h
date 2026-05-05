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

typedef struct {
  char *ui_result;
  char *raw_result;
} PluginResult;

typedef struct {
  Plugin **plugins; // Массив указателей на плагины
  int count;        // Сколько всего плагинов загружено
  int capacity;     // Сколько памяти выделено
} PluginRegistry;

void plugin_registry_add(Plugin *plugin);

Plugin *plugin_registry_find(const char *command);

void plugin_registry_cleanup(void);

void plugins_init(void);

Plugin *plugin_load(const char *path);

PluginResult *plugin_execute_sync(Plugin *plugin, const char *input);

void plugins_cleanup();
char *get_plugins_info();
#endif
