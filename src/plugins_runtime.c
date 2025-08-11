#include "mcp.h"
#include "plugins.h"
#include "plugins_internal.h"
#include "agent.h"
#include "app_config.h"
#include "embedded_assets.h"
#include "http.h"
#include "log.h"
#include "permit.h"
#include "popup.h"
#include "project_instructions.h"
#include "skills.h"
#include "wiki.h"
#include <errno.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

lua_State *L = NULL;

static int lua_doasset_or_file(lua_State *l, const char *asset_path,
                               const char *override_path) {
  if (override_path && plugins_file_exists(override_path))
    return luaL_dofile(l, override_path);

  const EmbeddedAsset *asset = embedded_asset_find(asset_path);
  if (!asset) {
    lua_pushfstring(l, "missing embedded asset: %s", asset_path);
    return LUA_ERRFILE;
  }

  return plugins_lua_dobuffer_named(l, asset_path, asset->data, asset->size);
}

static int l_require_embedded_json(lua_State *l) {
  const EmbeddedAsset *asset = embedded_asset_find("vendor/rxi/json.lua");
  if (!asset)
    return luaL_error(l, "missing embedded asset: vendor/rxi/json.lua");

  lua_settop(l, 0);
  if (plugins_lua_dobuffer_named(l, "vendor/rxi/json.lua", asset->data,
                                 asset->size) != LUA_OK) {
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
  if (plugins_lua_dobuffer_named(l, asset_path, asset->data, asset->size) !=
      LUA_OK) {
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
  preload_embedded_asset(L, "agent.provider_config",
                         "agent/provider_config.lua");
  preload_embedded_asset(L, "agent.models", "agent/models.lua");
  preload_embedded_asset(L, "agent.stream", "agent/stream.lua");
  preload_embedded_asset(L, "agent.tools", "agent/tools.lua");
  preload_embedded_asset(L, "agent.workspace", "agent/workspace.lua");
  preload_embedded_asset(L, "agent.redact", "agent/redact.lua");
  preload_embedded_asset(L, "agent.tokens", "agent/tokens.lua");
  preload_embedded_asset(L, "agent.images", "agent/images.lua");
  preload_embedded_asset(L, "agent.logging", "agent/logging.lua");
  preload_embedded_asset(L, "agent.utf8", "agent/utf8.lua");
  preload_embedded_asset(L, "agent.hooks", "agent/hooks.lua");
  preload_embedded_asset(L, "agent.state", "agent/state.lua");
  preload_embedded_asset(L, "agent.auth", "agent/auth.lua");
  preload_embedded_asset(L, "agent.lua_serialize", "agent/lua_serialize.lua");
  preload_embedded_asset(L, "agent.shell_safe", "agent/shell_safe.lua");
  preload_embedded_asset(L, "agent.mcp", "agent/mcp.lua");
  preload_embedded_asset(L, "agent.profiles", "agent/profiles.lua");
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
                                     size_t builtin_skill_capacity,
                                     int disable_wiki) {
  size_t count = 0;
  if (self_improvement_allowed() && count < builtin_skill_capacity) {
    const EmbeddedAsset *asset =
        embedded_asset_find("skills/self-improvement/SKILL.md");
    if (asset) {
      builtin_skills[count++] = (BuiltinSkill){
          .name = "self-improvement",
          .path = "embedded:skills/self-improvement/SKILL.md",
          .content = asset->data,
          .content_size = asset->size,
      };
    }
  }

  if (!disable_wiki && count < builtin_skill_capacity) {
    const EmbeddedAsset *asset =
        embedded_asset_find("skills/wiki-onboarding/SKILL.md");
    if (asset) {
      builtin_skills[count++] = (BuiltinSkill){
          .name = "wiki-onboarding",
          .path = "embedded:skills/wiki-onboarding/SKILL.md",
          .content = asset->data,
          .content_size = asset->size,
      };
    }
  }

  return count;
}

static void load_system_prompt(const PluginsInitOptions *options) {
  const EmbeddedAsset *asset = embedded_asset_find("ai/system_prompt.txt");
  const char *data = asset ? asset->data : "";
  size_t size = asset ? asset->size : 0;
  char *override = NULL;

  char path[512];
  int isolated = options && options->isolated;
  if (!isolated && app_config_path(path, sizeof(path), "system_prompt.txt") == 0) {
    override = plugins_read_file(path, &size);
    if (override)
      data = override;
  }

  char project_skills[512];
  char user_skills[512];
  char common_skills[512];
  BuiltinSkill builtin_skills[2];
  int disable_wiki = options && options->disable_wiki;
  cleanup_materialized_builtin_skills();
  size_t builtin_skill_count = isolated ? 0 :
      collect_builtin_skills(builtin_skills,
                             sizeof(builtin_skills) / sizeof(builtin_skills[0]),
                             disable_wiki);
  int n = snprintf(project_skills, sizeof(project_skills), "%s/.agents/skills",
                   app_workspace_root());
  const char *project_skills_dir = !isolated &&
      n > 0 && (size_t)n < sizeof(project_skills) ? project_skills : NULL;
  const char *user_skills_dir = !isolated &&
      app_config_path(user_skills, sizeof(user_skills), "skills") == 0
          ? user_skills
          : NULL;
  const char *home = getenv("HOME");
  const char *common_skills_dir = NULL;
  if (!isolated && home) {
    int common_n = snprintf(common_skills, sizeof(common_skills),
                            "%s/.agents/skills", home);
    if (common_n > 0 && (size_t)common_n < sizeof(common_skills))
      common_skills_dir = common_skills;
  }
  char *skills_prompt =
      skills_build_prompt(builtin_skills, builtin_skill_count, project_skills_dir,
                          user_skills_dir, common_skills_dir);
  char *skills_summary =
      skills_build_summary(builtin_skills, builtin_skill_count,
                           project_skills_dir, user_skills_dir,
                           common_skills_dir);

  const char *wiki_path = NULL;
  char default_wiki_path[512];
  default_wiki_path[0] = '\0';
  if (!disable_wiki) {
    lua_getglobal(L, "capstan");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "config");
      if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "wiki");
        if (lua_istable(L, -1)) {
          lua_getfield(L, -1, "path");
          if (lua_isstring(L, -1))
            wiki_path = lua_tostring(L, -1);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!wiki_path &&
        app_state_path(default_wiki_path, sizeof(default_wiki_path), "wiki") ==
            0) {
      if (app_state_ensure_dir() == 0)
        mkdir(default_wiki_path, 0755);
      wiki_path = default_wiki_path;
    }
  }
  char *wiki_prompt = disable_wiki ? NULL : wiki_build_prompt(wiki_path);
  char *wiki_summary = disable_wiki ? NULL : wiki_build_summary(wiki_path);

  char config_dir[512];
  const char *instruction_config_dir =
      app_config_dir(config_dir, sizeof(config_dir)) == 0 ? config_dir : NULL;
  char *instructions_prompt = isolated
      ? NULL
      : project_instructions_build_prompt(app_workspace_root(),
                                          instruction_config_dir, home);
  size_t instructions_size =
      instructions_prompt ? strlen(instructions_prompt) : 0;

  size_t skills_size = skills_prompt ? strlen(skills_prompt) : 0;
  size_t wiki_size = wiki_prompt ? strlen(wiki_prompt) : 0;
  char *combined =
      malloc(size + instructions_size + skills_size + wiki_size + 1);
  if (combined) {
    size_t pos = 0;
    memcpy(combined + pos, data, size);
    pos += size;
    if (instructions_size) {
      memcpy(combined + pos, instructions_prompt, instructions_size);
      pos += instructions_size;
    }
    if (skills_size) {
      memcpy(combined + pos, skills_prompt, skills_size);
      pos += skills_size;
    }
    if (wiki_size) {
      memcpy(combined + pos, wiki_prompt, wiki_size);
      pos += wiki_size;
    }
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

    if (!disable_wiki && wiki_summary) {
      lua_pushstring(L, wiki_summary);
      lua_setfield(L, -2, "wiki_summary");
    }

    if (!disable_wiki && wiki_path) {
      lua_pushstring(L, wiki_path);
      lua_setfield(L, -2, "wiki_path");
    }

    lua_newtable(L);
    int skill_root_index = 1;
    if (project_skills_dir) {
      lua_pushstring(L, project_skills_dir);
      lua_rawseti(L, -2, skill_root_index++);
    }
    if (user_skills_dir) {
      lua_pushstring(L, user_skills_dir);
      lua_rawseti(L, -2, skill_root_index++);
    }
    if (common_skills_dir) {
      lua_pushstring(L, common_skills_dir);
      lua_rawseti(L, -2, skill_root_index++);
    }
    lua_setfield(L, -2, "skill_roots");
  }
  lua_pop(L, 1);

  free(combined);
  free(instructions_prompt);
  free(wiki_summary);
  free(wiki_prompt);
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

static int l_capstan_realpath(lua_State *l) {
  const char *path = luaL_checkstring(l, 1);
  char resolved[PATH_MAX];
  if (!realpath(path, resolved)) {
    lua_pushnil(l);
    lua_pushstring(l, strerror(errno));
    return 2;
  }
  lua_pushstring(l, resolved);
  return 1;
}

static int l_capstan_embedded_asset(lua_State *l) {
  const char *path = luaL_checkstring(l, 1);
  if (strncmp(path, "embedded:", 9) == 0)
    path += 9;
  if (!path[0]) {
    lua_pushnil(l);
    lua_pushstring(l, "missing embedded asset path");
    return 2;
  }
  const EmbeddedAsset *asset = embedded_asset_find(path);
  if (!asset) {
    lua_pushnil(l);
    lua_pushfstring(l, "missing embedded asset: %s", path);
    return 2;
  }
  lua_pushlstring(l, asset->data, asset->size);
  return 1;
}

static int write_all_fd(int fd, const char *data, size_t size) {
  size_t written = 0;
  while (written < size) {
    ssize_t n = write(fd, data + written, size - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    written += (size_t)n;
  }
  return 0;
}

static int l_capstan_secure_write_file(lua_State *l) {
  const char *path = luaL_checkstring(l, 1);
  size_t content_size = 0;
  const char *content = luaL_checklstring(l, 2, &content_size);
  char tmp_path[PATH_MAX];
  int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
    lua_pushboolean(l, 0);
    lua_pushstring(l, "temporary path is too long");
    return 2;
  }

  unlink(tmp_path);
  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    lua_pushboolean(l, 0);
    lua_pushstring(l, strerror(errno));
    return 2;
  }
  if (fchmod(fd, 0600) != 0) {
    char err[256];
    snprintf(err, sizeof(err), "%s", strerror(errno));
    close(fd);
    unlink(tmp_path);
    lua_pushboolean(l, 0);
    lua_pushstring(l, err);
    return 2;
  }

  if (write_all_fd(fd, content, content_size) != 0) {
    char err[256];
    snprintf(err, sizeof(err), "%s", strerror(errno));
    close(fd);
    unlink(tmp_path);
    lua_pushboolean(l, 0);
    lua_pushstring(l, err);
    return 2;
  }
  if (close(fd) != 0) {
    char err[256];
    snprintf(err, sizeof(err), "%s", strerror(errno));
    unlink(tmp_path);
    lua_pushboolean(l, 0);
    lua_pushstring(l, err);
    return 2;
  }
  if (rename(tmp_path, path) != 0) {
    char err[256];
    snprintf(err, sizeof(err), "%s", strerror(errno));
    unlink(tmp_path);
    lua_pushboolean(l, 0);
    lua_pushstring(l, err);
    return 2;
  }

  lua_pushboolean(l, 1);
  return 1;
}

static void register_capstan_runtime(const PluginsInitOptions *options) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }

  lua_pushstring(L, app_workdir());
  lua_setfield(L, -2, "workdir");

  lua_pushstring(L, app_workspace_root());
  lua_setfield(L, -2, "workspace_root");

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

  lua_pushcfunction(L, l_capstan_realpath);
  lua_setfield(L, -2, "realpath");

  lua_pushcfunction(L, l_capstan_embedded_asset);
  lua_setfield(L, -2, "embedded_asset");

  lua_pushcfunction(L, l_capstan_secure_write_file);
  lua_setfield(L, -2, "secure_write_file");

  lua_newtable(L);
  lua_pushboolean(L, options && options->disable_mcp);
  lua_setfield(L, -2, "disable_mcp");
  lua_pushboolean(L, options && options->disable_wiki);
  lua_setfield(L, -2, "disable_wiki");
  lua_pushboolean(L, options && options->isolated);
  lua_setfield(L, -2, "isolated");
  lua_setfield(L, -2, "runtime_options");

  lua_setglobal(L, "capstan");
}

static void load_capstan_config(void) {
  char path[512];
  if (app_config_path(path, sizeof(path), "config.lua") != 0)
    return;
  if (!plugins_file_exists(path))
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
      plugins_file_exists(path)) {
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

void plugins_init_with_options(const PluginsInitOptions *options) {
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
    snprintf(new_path, sizeof(new_path), "%s;%s;./?.lua;plugins/?.lua", cur,
             alter_path);
    lua_pushstring(L, new_path);
    lua_setfield(L, -3, "path");
    lua_pop(L, 2);
  }

  register_embedded_modules();

  http_init(L);
  agent_init(L);
  tools_init(L);
  mcp_init(L);
  register_capstan_runtime(options);
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

  load_system_prompt(options);

  if (lua_doasset_or_file(L, "agent/runtime.lua", NULL) != LUA_OK) {
    popup_show_message("Startup Error", lua_tostring(L, -1), 1);
    lua_pop(L, 1);
  }
}

void plugins_init(void) { plugins_init_with_options(NULL); }

void plugins_cleanup(void) {
  if (L) {
    log_cleanup();
    lua_close(L);
    L = NULL;
  }
  mcp_cleanup();
  http_cleanup();
}
