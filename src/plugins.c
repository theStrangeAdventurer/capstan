#include "mcp.h"
#include "plugins.h"
#include "agent.h"
#include "app_config.h"
#include "embedded_assets.h"
#include "http.h"
#include "log.h"
#include "permit.h"
#include "popup.h"
#include "skills.h"
#include "utils.h"
#include <dirent.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PLUGIN_RESPONSE_MAX_SIZE 4096
#define PLUGIN_CAPACITY_INCREMENT 10

lua_State *L = NULL;

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_file(const char *path, size_t *out_size) {
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

static int lua_dobuffer_named(lua_State *l, const char *name, const char *data,
                              size_t size) {
  if (luaL_loadbuffer(l, data, size, name) != LUA_OK)
    return LUA_ERRSYNTAX;
  return lua_pcall(l, 0, LUA_MULTRET, 0);
}

static int lua_doasset_or_file(lua_State *l, const char *asset_path,
                               const char *override_path) {
  if (override_path && file_exists(override_path))
    return luaL_dofile(l, override_path);

  const EmbeddedAsset *asset = embedded_asset_find(asset_path);
  if (!asset) {
    lua_pushfstring(l, "missing embedded asset: %s", asset_path);
    return LUA_ERRFILE;
  }

  return lua_dobuffer_named(l, asset_path, asset->data, asset->size);
}

static int l_require_embedded_json(lua_State *l) {
  const EmbeddedAsset *asset = embedded_asset_find("vendor/rxi/json.lua");
  if (!asset)
    return luaL_error(l, "missing embedded asset: vendor/rxi/json.lua");

  lua_settop(l, 0);
  if (lua_dobuffer_named(l, "vendor/rxi/json.lua", asset->data, asset->size) !=
      LUA_OK) {
    const char *err = lua_tostring(l, -1);
    return luaL_error(l, "%s", err ? err : "failed to load vendor.rxi.json");
  }

  return lua_gettop(l);
}

static int l_require_embedded_asset(lua_State *l) {
  const char *asset_path = (const char *)lua_touserdata(l, lua_upvalueindex(1));
  const EmbeddedAsset *asset = embedded_asset_find(asset_path);
  if (!asset)
    return luaL_error(l, "missing embedded asset: %s", asset_path);

  lua_settop(l, 0);
  if (lua_dobuffer_named(l, asset_path, asset->data, asset->size) != LUA_OK) {
    const char *err = lua_tostring(l, -1);
    return luaL_error(l, "%s", err ? err : "failed to load embedded asset");
  }
  return lua_gettop(l);
}

static void preload_embedded_asset(lua_State *l, const char *module,
                                   const char *asset_path) {
  lua_pushlightuserdata(l, (void *)asset_path);
  lua_pushcclosure(l, l_require_embedded_asset, 1);
  lua_setfield(l, -2, module);
}

static void register_embedded_modules(void) {
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "preload");
  lua_pushcfunction(L, l_require_embedded_json);
  lua_setfield(L, -2, "vendor.rxi.json");
  preload_embedded_asset(L, "agent.runtime", "agent/runtime.lua");
  preload_embedded_asset(L, "agent.provider_config", "agent/provider_config.lua");
  preload_embedded_asset(L, "agent.models", "agent/models.lua");
  preload_embedded_asset(L, "agent.stream", "agent/stream.lua");
  preload_embedded_asset(L, "agent.tools", "agent/tools.lua");
  preload_embedded_asset(L, "agent.tokens", "agent/tokens.lua");
  preload_embedded_asset(L, "agent.logging", "agent/logging.lua");
  preload_embedded_asset(L, "agent.hooks", "agent/hooks.lua");
  preload_embedded_asset(L, "agent.state", "agent/state.lua");
  preload_embedded_asset(L, "agent.shell_safe", "agent/shell_safe.lua");
  preload_embedded_asset(L, "agent.mcp", "agent/mcp.lua");
  lua_pop(L, 2);
}

static int capstan_config_bool(const char *section, const char *field) {
  int result = 0;
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return 0;
  }
  lua_getfield(L, -1, section);
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, field);
    result = lua_isboolean(L, -1) && lua_toboolean(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 3);
  return result;
}

static int self_improvement_allowed(void) {
  return capstan_config_bool("capabilities", "self_improvement");
}

static void cleanup_materialized_builtin_skills(void) {
  char skill_path[512];
  if (app_state_path(skill_path, sizeof(skill_path),
                     "builtin-skills/self-improvement/SKILL.md") != 0)
    return;

  unlink(skill_path);

  char self_improvement_dir[512];
  if (app_state_path(self_improvement_dir, sizeof(self_improvement_dir),
                     "builtin-skills/self-improvement") == 0)
    rmdir(self_improvement_dir);

  char builtin_skills_dir[512];
  if (app_state_path(builtin_skills_dir, sizeof(builtin_skills_dir),
                     "builtin-skills") == 0)
    rmdir(builtin_skills_dir);
}

static size_t collect_builtin_skills(BuiltinSkill *builtin_skills,
                                     size_t builtin_skill_capacity) {
  size_t count = 0;
  if (!self_improvement_allowed())
    return 0;
  if (count >= builtin_skill_capacity)
    return count;

  const EmbeddedAsset *asset =
      embedded_asset_find("skills/self-improvement/SKILL.md");
  if (!asset)
    return count;

  builtin_skills[count++] = (BuiltinSkill){
      .name = "self-improvement",
      .path = "embedded:skills/self-improvement/SKILL.md",
      .content = asset->data,
      .content_size = asset->size,
  };
  return count;
}

static void load_system_prompt(void) {
  const EmbeddedAsset *asset = embedded_asset_find("ai/system_prompt.txt");
  const char *data = asset ? asset->data : "";
  size_t size = asset ? asset->size : 0;
  char *override = NULL;

  char path[512];
  if (app_config_path(path, sizeof(path), "system_prompt.txt") == 0) {
    override = read_file(path, &size);
    if (override)
      data = override;
  }

  char project_skills[512];
  char user_skills[512];
  char common_skills[512];
  BuiltinSkill builtin_skills[1];
  cleanup_materialized_builtin_skills();
  size_t builtin_skill_count =
      collect_builtin_skills(builtin_skills,
                             sizeof(builtin_skills) / sizeof(builtin_skills[0]));
  int n = snprintf(project_skills, sizeof(project_skills), "%s/.agents/skills",
                   app_workdir());
  const char *project_skills_dir =
      n > 0 && (size_t)n < sizeof(project_skills) ? project_skills : NULL;
  const char *user_skills_dir =
      app_config_path(user_skills, sizeof(user_skills), "skills") == 0
          ? user_skills
          : NULL;
  const char *home = getenv("HOME");
  const char *common_skills_dir = NULL;
  if (home) {
    int common_n = snprintf(common_skills, sizeof(common_skills),
                            "%s/.agents/skills", home);
    if (common_n > 0 && (size_t)common_n < sizeof(common_skills))
      common_skills_dir = common_skills;
  }
  char *skills_prompt =
      skills_build_prompt(builtin_skills, builtin_skill_count,
                          project_skills_dir,
                          user_skills_dir,
                          common_skills_dir);
  char *skills_summary =
      skills_build_summary(builtin_skills, builtin_skill_count,
                           project_skills_dir,
                           user_skills_dir,
                           common_skills_dir);

  char agents_path[512];
  char *agents_content = NULL;
  size_t agents_content_size = 0;
  int agents_n =
      snprintf(agents_path, sizeof(agents_path), "%s/AGENTS.md", app_workdir());
  if (agents_n > 0 && (size_t)agents_n < sizeof(agents_path))
    agents_content = read_file(agents_path, &agents_content_size);

  char *agents_prompt = NULL;
  size_t agents_prompt_size = 0;
  if (agents_content) {
    const char *prefix = "\n\n# Project Instructions\nPath: ";
    const char *middle = "\n\n";
    agents_prompt_size = strlen(prefix) + strlen(agents_path) +
                         strlen(middle) + agents_content_size;
    agents_prompt = malloc(agents_prompt_size + 1);
    if (agents_prompt) {
      size_t pos = 0;
      memcpy(agents_prompt + pos, prefix, strlen(prefix));
      pos += strlen(prefix);
      memcpy(agents_prompt + pos, agents_path, strlen(agents_path));
      pos += strlen(agents_path);
      memcpy(agents_prompt + pos, middle, strlen(middle));
      pos += strlen(middle);
      memcpy(agents_prompt + pos, agents_content, agents_content_size);
      pos += agents_content_size;
      agents_prompt[pos] = '\0';
    } else {
      agents_prompt_size = 0;
    }
  }

  size_t skills_size = skills_prompt ? strlen(skills_prompt) : 0;
  char *combined = malloc(size + agents_prompt_size + skills_size + 1);
  if (combined) {
    size_t pos = 0;
    memcpy(combined + pos, data, size);
    pos += size;
    if (agents_prompt_size) {
      memcpy(combined + pos, agents_prompt, agents_prompt_size);
      pos += agents_prompt_size;
    }
    if (skills_size)
      memcpy(combined + pos, skills_prompt, skills_size);
    pos += skills_size;
    combined[pos] = '\0';
    lua_pushlstring(L, combined, pos);
  } else {
    lua_pushlstring(L, data, size);
  }
  lua_setglobal(L, "system_prompt");

  lua_getglobal(L, "capstan");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, skills_summary ? skills_summary : "No skills loaded.");
    lua_setfield(L, -2, "skills_summary");
  }
  lua_pop(L, 1);

  free(combined);
  free(agents_prompt);
  free(agents_content);
  free(skills_summary);
  free(skills_prompt);
  free(override);
}

static int l_popup_info(lua_State *l) {
  popup_show_message(luaL_checkstring(l, 1), luaL_checkstring(l, 2), 0);
  return 0;
}

static int l_popup_error(lua_State *l) {
  popup_show_message(luaL_checkstring(l, 1), luaL_checkstring(l, 2), 1);
  return 0;
}

static int l_capstan_state_path(lua_State *l) {
  const char *relative = luaL_optstring(l, 1, "state.lua");
  char path[512];
  if (app_state_path(path, sizeof(path), relative) != 0) {
    lua_pushnil(l);
    return 1;
  }
  lua_pushstring(l, path);
  return 1;
}

static int l_capstan_state_dir(lua_State *l) {
  char path[512];
  if (app_state_dir(path, sizeof(path)) != 0) {
    lua_pushnil(l);
    return 1;
  }
  lua_pushstring(l, path);
  return 1;
}

static int l_capstan_config_path(lua_State *l) {
  const char *relative = luaL_optstring(l, 1, "config.lua");
  char path[512];
  if (app_config_path(path, sizeof(path), relative) != 0) {
    lua_pushnil(l);
    return 1;
  }
  lua_pushstring(l, path);
  return 1;
}

static int l_capstan_config_dir(lua_State *l) {
  char path[512];
  if (app_config_dir(path, sizeof(path)) != 0) {
    lua_pushnil(l);
    return 1;
  }
  lua_pushstring(l, path);
  return 1;
}

static int l_capstan_state_ensure_dir(lua_State *l) {
  lua_pushboolean(l, app_state_ensure_dir() == 0);
  return 1;
}

static int l_capstan_now_ms(lua_State *l) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  lua_pushnumber(l, (lua_Number)tv.tv_sec * 1000.0 +
                        (lua_Number)tv.tv_usec / 1000.0);
  return 1;
}

static void register_capstan_runtime(void) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }

  lua_pushstring(L, app_workdir());
  lua_setfield(L, -2, "workdir");

  lua_pushcfunction(L, l_capstan_state_path);
  lua_setfield(L, -2, "state_path");

  lua_pushcfunction(L, l_capstan_state_dir);
  lua_setfield(L, -2, "state_dir");

  lua_pushcfunction(L, l_capstan_config_path);
  lua_setfield(L, -2, "config_path");

  lua_pushcfunction(L, l_capstan_config_dir);
  lua_setfield(L, -2, "config_dir");

  lua_pushcfunction(L, l_capstan_state_ensure_dir);
  lua_setfield(L, -2, "state_ensure_dir");

  lua_pushcfunction(L, l_capstan_now_ms);
  lua_setfield(L, -2, "now_ms");

  lua_setglobal(L, "capstan");
}

static void load_capstan_config(void) {
  char path[512];
  if (app_config_path(path, sizeof(path), "config.lua") != 0)
    return;
  if (!file_exists(path))
    return;

  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }

  if (luaL_dofile(L, path) != LUA_OK) {
    fprintf(stderr, "Error loading config.lua: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "capstan");
    lua_pop(L, 1);
    return;
  }

  if (lua_istable(L, -1)) {
    lua_setfield(L, -2, "config");
  } else {
    lua_pop(L, 1);
  }
  lua_pushvalue(L, -1);
  lua_setglobal(L, "capstan");
  lua_pop(L, 1);
}

static void load_capstan_state(void) {
  char path[512];
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }

  if (app_state_path(path, sizeof(path), "state.lua") == 0 &&
      file_exists(path)) {
    if (luaL_dofile(L, path) != LUA_OK) {
      fprintf(stderr, "Error loading state.lua: %s\n", lua_tostring(L, -1));
      lua_pop(L, 1);
      lua_newtable(L);
    } else if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
    }
  } else {
    lua_newtable(L);
  }

  lua_setfield(L, -2, "state");
  lua_pushvalue(L, -1);
  lua_setglobal(L, "capstan");
  lua_pop(L, 1);
}

void plugins_init(void) {
  L = luaL_newstate();
  luaL_openlibs(L);

  const char *home = getenv("HOME");
  if (home) {
    char alter_path[2048];
    snprintf(alter_path, sizeof(alter_path), "%s/.config/%s/?.lua", home,
             APP_CONFIG_DIR_NAME);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char *cur = lua_tostring(L, -1);
    char new_path[4096];
    snprintf(new_path, sizeof(new_path), "%s;%s;./?.lua;plugins/?.lua",
             cur, alter_path);
    lua_pushstring(L, new_path);
    lua_setfield(L, -3, "path");
    lua_pop(L, 2);
  }

  register_embedded_modules();

  http_init(L);
  agent_init(L);
  tools_init(L);
  mcp_init(L);
  register_capstan_runtime();
  load_capstan_config();
  load_capstan_state();
  permit_init(L);
  log_init(L);

  lua_newtable(L);
  lua_pushcfunction(L, l_popup_info);
  lua_setfield(L, -2, "info");
  lua_pushcfunction(L, l_popup_error);
  lua_setfield(L, -2, "error");
  lua_setglobal(L, "popup");

  load_system_prompt();

  if (lua_doasset_or_file(L, "agent/runtime.lua", NULL) != LUA_OK) {
    popup_show_message("Startup Error", lua_tostring(L, -1), 1);
    lua_pop(L, 1);
  }
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
    int new_capacity = plugins_registry.capacity + PLUGIN_CAPACITY_INCREMENT;
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
  // Достаем из таблицы плагина поле id и после этого это
  // поле будет наверху стека (-1) а таблица будет уже (-2)
  // если бы искали поле в таблице с индексом -2 - значение полч потом все равно
  // бы оказалось на вершине стека
  lua_getfield(L, idx, field);

  const char *strval = lua_tostring(L, -1); // Теперь берем с вершины стека
  char *result = strval ? my_strdup(strval)
                        : (fallback ? my_strdup(fallback) : NULL);
  lua_pop(L, 1); // Снимаем  id со стека, теперь снова таблица на -1
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
  if (lua_dobuffer_named(L, name, data, size) !=
      LUA_OK) { // Выполняет lua файл и кладет результат выполнения на стек lua
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "Error loading plugin: %s\n", err);
    record_plugin_error(name, err);
    lua_pop(L, 1);
    return NULL;
  }
  if (!lua_istable(L, -1)) { // Проверяем самый верхний элемент на стеке
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
    // Автоматически снимает с вершины стека функцию handler и сохраняет ее в
    // специальном реестре возвращая при этом ссылку (int) на нее
    // позже мы получим функцию по этой ссылке числу с помощью lua_rawgeti
    // LUA_REGISTRYINDEX - специальная таблица в которой можно безопасно хранить
    // данные lua чтобы их не собрал сборщик мусора
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
  char *data = read_file(path, &size);
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
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_watch_dir, entry->d_name);
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

void plugins_cleanup(void) {
  if (L) {
    lua_close(L);
    L = NULL;
  }
  mcp_cleanup();
  http_cleanup();
}
