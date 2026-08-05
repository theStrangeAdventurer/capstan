#include "acp.h"
#include "app_config.h"
#include "embedded_assets.h"
#include "http.h"
#include "plugins.h"
#include <errno.h>
#include <lauxlib.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_input_eof = 0;
static FILE *g_protocol_output = NULL;
static char g_acp_workspace[PATH_MAX] = "";

static int handle_line(lua_State *l, const char *line, size_t len);
static long read_ndjson_line(char **line, size_t *capacity);
static int read_request(lua_State *l, char **line, size_t *capacity);

static int l_acp_send(lua_State *l) {
  size_t len = 0;
  const char *line = luaL_checklstring(l, 1, &len);
  FILE *output = g_protocol_output ? g_protocol_output : stdout;
  if (fwrite(line, 1, len, output) != len || fputc('\n', output) == EOF ||
      fflush(output) != 0)
    return luaL_error(l, "could not write ACP response");
  return 0;
}

static int l_acp_cancel(lua_State *l) {
  lua_pushinteger(l, http_cancel_streams(l));
  return 1;
}

static int l_acp_pump(lua_State *l) {
  struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN | POLLHUP};
  int ready = poll(&input, 1, 0);
  if (ready < 0 && errno != EINTR)
    return luaL_error(l, "ACP poll failed: %s", strerror(errno));
  if (ready > 0 && (input.revents & POLLIN)) {
    char *line = NULL;
    size_t capacity = 0;
    read_request(l, &line, &capacity);
    free(line);
  }
  if (ready > 0 && (input.revents & POLLHUP) && !(input.revents & POLLIN))
    g_input_eof = 1;
  lua_pushboolean(l, !g_input_eof);
  return 1;
}

static int l_acp_wait_response(lua_State *l) {
  lua_Integer request_id = luaL_checkinteger(l, 1);
  char *line = NULL;
  size_t capacity = 0;

  while (!g_input_eof) {
    long n = read_ndjson_line(&line, &capacity);
    if (n < 0) {
      if (n == -1)
        g_input_eof = 1;
      free(line);
      lua_pushnil(l);
      return 1;
    }
    while (n > 0 && line[n - 1] == '\r')
      n--;
    if (n > 0 && !handle_line(l, line, (size_t)n)) {
      free(line);
      lua_pushnil(l);
      return 1;
    }

    lua_getglobal(l, "capstan_acp_take_response");
    if (lua_isfunction(l, -1)) {
      lua_pushinteger(l, request_id);
      if (lua_pcall(l, 1, 1, 0) == LUA_OK) {
        if (!lua_isnil(l, -1)) {
          free(line);
          return 1;
        }
        lua_pop(l, 1);
      } else {
        fprintf(stderr, "capstan: ACP response handling failed: %s\n",
                lua_tostring(l, -1));
        lua_pop(l, 1);
      }
    } else {
      lua_pop(l, 1);
    }

    lua_getglobal(l, "capstan_acp_active");
    if (lua_isfunction(l, -1) && lua_pcall(l, 0, 1, 0) == LUA_OK) {
      int active = lua_toboolean(l, -1);
      lua_pop(l, 1);
      if (!active) {
        free(line);
        lua_pushnil(l);
        return 1;
      }
    } else {
      lua_pop(l, 1);
    }
  }

  free(line);
  lua_pushnil(l);
  return 1;
}

static void set_capstan_path(lua_State *l, const char *field,
                             const char *value) {
  lua_getglobal(l, "capstan");
  if (lua_istable(l, -1)) {
    lua_pushstring(l, value);
    lua_setfield(l, -2, field);
  }
  lua_pop(l, 1);
}

static int path_is_within(const char *path, const char *root) {
  size_t root_len = strlen(root);
  return strcmp(path, root) == 0 ||
         (strcmp(root, "/") == 0 && path[0] == '/') ||
         (root_len > 0 && strncmp(path, root, root_len) == 0 &&
          path[root_len] == '/');
}

static int l_acp_set_directory(lua_State *l) {
  const char *cwd = luaL_checkstring(l, 1);
  char resolved[PATH_MAX];
  if (cwd[0] != '/' || !realpath(cwd, resolved)) {
    lua_pushboolean(l, 0);
    lua_pushliteral(l, "cwd must be an existing absolute directory");
    return 2;
  }
  if (!g_acp_workspace[0] || !path_is_within(resolved, g_acp_workspace)) {
    lua_pushboolean(l, 0);
    lua_pushfstring(l,
                    "ACP process is bound to workspace %s; start a separate "
                    "capstan acp process from %s",
                    g_acp_workspace[0] ? g_acp_workspace : "(unknown)", resolved);
    return 2;
  }
  if (!app_workdir_set(resolved) || !app_workspace_set(resolved)) {
    lua_pushboolean(l, 0);
    lua_pushliteral(l, "could not activate ACP session workspace");
    return 2;
  }
  set_capstan_path(l, "workdir", app_workdir());
  set_capstan_path(l, "workspace_root", app_workspace_root());
  lua_pushboolean(l, 1);
  return 1;
}

static int l_acp_commands(lua_State *l) {
  lua_newtable(l);
  int out = 1;
  for (int i = 0; i < plugin_registry_count(); i++) {
    Plugin *plugin = plugin_registry_at(i);
    if (!plugin || !plugin->command || plugin->command[0] != '/')
      continue;
    lua_newtable(l);
    lua_pushstring(l, plugin->command + 1);
    lua_setfield(l, -2, "name");
    lua_pushstring(l, plugin->description ? plugin->description : "");
    lua_setfield(l, -2, "description");
    lua_rawseti(l, -2, out++);
  }
  return 1;
}

void acp_register(lua_State *l) {
  lua_newtable(l);
  lua_pushcfunction(l, l_acp_send);
  lua_setfield(l, -2, "send");
  lua_pushcfunction(l, l_acp_cancel);
  lua_setfield(l, -2, "cancel");
  lua_pushcfunction(l, l_acp_pump);
  lua_setfield(l, -2, "pump");
  lua_pushcfunction(l, l_acp_wait_response);
  lua_setfield(l, -2, "wait_response");
  lua_pushcfunction(l, l_acp_set_directory);
  lua_setfield(l, -2, "set_directory");
  lua_pushcfunction(l, l_acp_commands);
  lua_setfield(l, -2, "commands");
  lua_pushstring(l, APP_VERSION);
  lua_setfield(l, -2, "version");
  lua_setglobal(l, "acp");
}

static int load_adapter(lua_State *l) {
  const EmbeddedAsset *asset = embedded_asset_find("agent/acp.lua");
  if (!asset) {
    fprintf(stderr, "capstan: missing embedded ACP adapter\n");
    return 0;
  }
  if (luaL_loadbuffer(l, asset->data, asset->size, "@agent/acp.lua") != LUA_OK ||
      lua_pcall(l, 0, 0, 0) != LUA_OK) {
    fprintf(stderr, "capstan: could not load ACP adapter: %s\n",
            lua_tostring(l, -1));
    lua_pop(l, 1);
    return 0;
  }
  return 1;
}

static int call_global_noargs(lua_State *l, const char *name, int results) {
  lua_getglobal(l, name);
  if (!lua_isfunction(l, -1)) {
    lua_pop(l, 1);
    return 0;
  }
  if (lua_pcall(l, 0, results, 0) != LUA_OK) {
    fprintf(stderr, "capstan: ACP %s failed: %s\n", name,
            lua_tostring(l, -1));
    lua_pop(l, 1);
    return 0;
  }
  return 1;
}

static int adapter_active(lua_State *l) {
  if (!call_global_noargs(l, "capstan_acp_active", 1))
    return 0;
  int active = lua_toboolean(l, -1);
  lua_pop(l, 1);
  return active;
}

static int handle_line(lua_State *l, const char *line, size_t len) {
  lua_getglobal(l, "capstan_acp_handle");
  if (!lua_isfunction(l, -1)) {
    lua_pop(l, 1);
    return 0;
  }
  lua_pushlstring(l, line, len);
  if (lua_pcall(l, 1, 0, 0) != LUA_OK) {
    fprintf(stderr, "capstan: ACP request failed: %s\n", lua_tostring(l, -1));
    lua_pop(l, 1);
    return 0;
  }
  return 1;
}

static long read_ndjson_line(char **line, size_t *capacity) {
  size_t size = 0;
  if (!*line || *capacity < 2) {
    *capacity = 4096;
    *line = malloc(*capacity);
    if (!*line)
      return -2;
  }

  while (1) {
    int ch = fgetc(stdin);
    if (ch == EOF) {
      if (ferror(stdin))
        return -2;
      if (size == 0)
        return -1;
      break;
    }
    if (ch == '\n')
      break;
    if (size + 1 >= *capacity) {
      if (*capacity > ((size_t)-1) / 2)
        return -2;
      size_t next = *capacity * 2;
      char *grown = realloc(*line, next);
      if (!grown)
        return -2;
      *line = grown;
      *capacity = next;
    }
    (*line)[size++] = (char)ch;
  }
  (*line)[size] = '\0';
  return (long)size;
}

static int read_request(lua_State *l, char **line, size_t *capacity) {
  errno = 0;
  long n = read_ndjson_line(line, capacity);
  if (n < 0) {
    if (n == -1)
      g_input_eof = 1;
    else
      fprintf(stderr, "capstan: ACP stdin read failed: %s\n",
              errno ? strerror(errno) : "out of memory");
    return 0;
  }
  while (n > 0 && (*line)[n - 1] == '\r')
    n--;
  if (n == 0)
    return 1;
  return handle_line(l, *line, (size_t)n);
}

int acp_run(const char *argv0) {
  int protocol_fd = dup(STDOUT_FILENO);
  if (protocol_fd < 0 || !(g_protocol_output = fdopen(protocol_fd, "w"))) {
    if (protocol_fd >= 0)
      close(protocol_fd);
    fprintf(stderr, "capstan: could not reserve ACP stdout\n");
    return 1;
  }
  if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    fprintf(stderr, "capstan: could not isolate ACP stdout\n");
    fclose(g_protocol_output);
    g_protocol_output = NULL;
    return 1;
  }

  const char *configured_workdir = getenv("CAPSTAN_WORKDIR");
  char launch_cwd[PATH_MAX];
  if ((!configured_workdir || !app_workdir_set(configured_workdir)) &&
      (!getcwd(launch_cwd, sizeof(launch_cwd)) ||
       !app_workdir_set(launch_cwd)))
    app_workdir_init(argv0);
  snprintf(g_acp_workspace, sizeof(g_acp_workspace), "%s",
           app_workspace_root());
  setlocale(LC_ALL, "");
  http_set_headless(1);

  PluginsInitOptions options = {0};
  plugins_init_with_options(&options);
  load_embedded_plugins();
  char global_plugins[512];
  if (app_config_path(global_plugins, sizeof(global_plugins), "plugins") == 0)
    load_plugins_from(global_plugins);

  acp_register(L);
  if (!load_adapter(L)) {
    plugins_cleanup();
    fclose(g_protocol_output);
    g_protocol_output = NULL;
    return 1;
  }

  char *line = NULL;
  size_t capacity = 0;
  int disconnected = 0;
  while (!g_input_eof || adapter_active(L) || http_is_loading()) {
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN | POLLHUP};
    int ready = g_input_eof ? 0 : poll(&input, 1, 10);
    if (ready < 0 && errno != EINTR) {
      fprintf(stderr, "capstan: ACP poll failed: %s\n", strerror(errno));
      break;
    }
    if (ready > 0 && (input.revents & POLLIN))
      read_request(L, &line, &capacity);
    if (ready > 0 && (input.revents & POLLHUP) && !(input.revents & POLLIN))
      g_input_eof = 1;

    http_poll_limited(L, 4);
    plugins_mcp_tick();

    if (g_input_eof && !disconnected) {
      call_global_noargs(L, "capstan_acp_disconnect", 0);
      disconnected = 1;
    }
  }

  free(line);
  plugins_cleanup();
  fclose(g_protocol_output);
  g_protocol_output = NULL;
  g_input_eof = 0;
  g_acp_workspace[0] = '\0';
  return 0;
}
