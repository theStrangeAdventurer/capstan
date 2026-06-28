#ifndef PLUGINS_H
#define PLUGINS_H

#include <lua.h>
#include "popup.h"

extern lua_State *L;

typedef struct Plugin Plugin;
struct Plugin {
  char *id;
  char *name;
  char *description;
  char *command;
  char *source_path;
  int include_in_history;
  int is_user_plugin;
  long source_mtime;
  long source_size;

  lua_State *L;
  int handler_ref;

  int has_autocomplete;
  int autocomplete_limit;
  int autocomplete_multi;
  char *autocomplete_title;
  int fetch_ref;

  int has_tool;
  char *tool_name;
  char *tool_desc;
  char *tool_params_json;
  char *tool_permission;
};

typedef struct {
  char *ui_result;
  char *raw_result;
} PluginResult;

typedef struct {
  Plugin **plugins;
  int count;
  int capacity;
} PluginRegistry;

void plugin_registry_add(Plugin *plugin);

Plugin *plugin_registry_find(const char *command);

void plugin_registry_cleanup(void);
int plugin_registry_count(void);
Plugin *plugin_registry_at(int index);

typedef struct {
  int disable_mcp;
} PluginsInitOptions;

void plugins_init(void);
void plugins_init_with_options(const PluginsInitOptions *options);

Plugin *plugin_load(const char *path);

PluginResult *plugin_execute(Plugin *plugin, const char *input, size_t cmd_end,
                             char **pre_args, int pre_arg_count);

int plugin_has_autocomplete(Plugin *plugin);
void plugin_autocomplete_fetch(Plugin *plugin, const char *input, size_t cmd_end,
                               PopupItem **out_items, int *out_count,
                               char **out_title, int *out_limit, int *out_multi);

void plugins_cleanup();
char *get_plugins_info();
void load_embedded_plugins(void);
void load_plugins_from(const char *dir_path);
void plugins_watch_start(const char *dir_path);
void plugins_watch_poll(void);
void plugin_registry_remove_by_id(const char *id);
#endif
