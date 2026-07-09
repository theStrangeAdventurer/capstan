#include "agent.h"
#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int stream_callback_ref = LUA_NOREF;
static char captured_body[8192];
static char captured_get_url[512];
static char captured_permit_tool[128];
static char captured_permit_target[512];
static char captured_logs[8192];
static char captured_agent_appends[2048];
static char last_agent_provider[128];
static char last_agent_model[128];
static char last_agent_profile[128];
static char temp_state_path[512];
static const char *permit_decision = "deny";
static const char *permit_prompt_decision = "always";
static char granted_tool[128];
static char granted_pattern[512];
static int grant_allow;
static int permit_prompt_calls;
static int permit_check_calls;
static int permit_save_calls;
static int mock_models_success;

static int l_http_get(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  strncpy(captured_get_url, url, sizeof(captured_get_url) - 1);
  captured_get_url[sizeof(captured_get_url) - 1] = '\0';
  if (mock_models_success) {
    lua_pushinteger(L, 200);
    lua_pushstring(L,
                   "{\"data\":["
                   "{\"id\":\"model/z\",\"name\":\"Model Z\","
                   "\"context_length\":12345},"
                   "{\"id\":\"model/a\",\"name\":\"Model A\"}"
                   "]}");
    return 2;
  }
  lua_pushinteger(L, 500);
  lua_pushstring(L, "");
  return 2;
}

static int l_http_post_stream(lua_State *L) {
  size_t body_len = 0;
  const char *body = luaL_checklstring(L, 2, &body_len);
  size_t copy = body_len < sizeof(captured_body) - 1
                    ? body_len
                    : sizeof(captured_body) - 1;
  memcpy(captured_body, body, copy);
  captured_body[copy] = '\0';

  if (stream_callback_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, stream_callback_ref);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  lua_pushvalue(L, 4);
  stream_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_pushinteger(L, 1);
  return 1;
}

static int l_permit_check(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *target = luaL_checkstring(L, 2);
  permit_check_calls++;
  strncpy(captured_permit_tool, tool, sizeof(captured_permit_tool) - 1);
  captured_permit_tool[sizeof(captured_permit_tool) - 1] = '\0';
  strncpy(captured_permit_target, target, sizeof(captured_permit_target) - 1);
  captured_permit_target[sizeof(captured_permit_target) - 1] = '\0';
  if (grant_allow && strcmp(tool, granted_tool) == 0 &&
      strcmp(target, granted_pattern) == 0) {
    lua_pushstring(L, "allow");
    return 1;
  }
  lua_pushstring(L, permit_decision);
  return 1;
}

static int l_permit_prompt(lua_State *L) {
  (void)L;
  permit_prompt_calls++;
  lua_pushstring(L, permit_prompt_decision);
  return 1;
}

static int l_permit_grant(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *pattern = luaL_checkstring(L, 2);
  strncpy(granted_tool, tool, sizeof(granted_tool) - 1);
  granted_tool[sizeof(granted_tool) - 1] = '\0';
  strncpy(granted_pattern, pattern, sizeof(granted_pattern) - 1);
  granted_pattern[sizeof(granted_pattern) - 1] = '\0';
  grant_allow = lua_toboolean(L, 3);
  return 0;
}

static int l_noop(lua_State *L) {
  (void)L;
  return 0;
}

static int l_permit_save(lua_State *L) {
  permit_save_calls++;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_http_post_response_async(lua_State *L) {
  size_t body_len = 0;
  const char *body = luaL_checklstring(L, 2, &body_len);
  size_t copy = body_len < sizeof(captured_body) - 1
                    ? body_len
                    : sizeof(captured_body) - 1;
  memcpy(captured_body, body, copy);
  captured_body[copy] = '\0';

  const char *method = strstr(captured_body, "\"method\":\"initialize\"")
                           ? "initialize"
                           : (strstr(captured_body, "\"method\":\"tools/list\"")
                                  ? "tools/list"
                                  : "notification");
  int id = 0;
  const char *id_pos = strstr(captured_body, "\"id\":");
  if (id_pos)
    id = atoi(id_pos + 5);

  luaL_checktype(L, 5, LUA_TFUNCTION);
  lua_pushvalue(L, 5);
  lua_newtable(L);
  lua_pushinteger(L, 200);
  lua_setfield(L, -2, "status");
  if (strcmp(method, "initialize") == 0) {
    lua_pushfstring(L,
                    "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"serverInfo\":{\"name\":\"github\",\"version\":\"1\"}}}",
                    id);
  } else if (strcmp(method, "tools/list") == 0) {
    lua_pushfstring(L,
                    "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"tools\":[{\"name\":\"search_repositories\",\"description\":\"Search repos\"}]}}",
                    id);
  } else {
    lua_pushliteral(L, "{}");
  }
  lua_setfield(L, -2, "body");
  lua_newtable(L);
  lua_pushliteral(L, "application/json");
  lua_setfield(L, -2, "content-type");
  lua_pushliteral(L, "session-1");
  lua_setfield(L, -2, "mcp-session-id");
  lua_setfield(L, -2, "headers");
  lua_pushnil(L);
  int rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_pushinteger(L, 1);
  return 1;
}

static int l_agent_append(lua_State *L) {
  const char *text = luaL_optstring(L, 1, "");
  size_t used = strlen(captured_agent_appends);
  if (used < sizeof(captured_agent_appends) - 1) {
    snprintf(captured_agent_appends + used,
             sizeof(captured_agent_appends) - used, "%s", text);
  }
  return 0;
}

static int l_agent_set_info(lua_State *L) {
  const char *provider = luaL_optstring(L, 1, "");
  const char *model = luaL_optstring(L, 2, "");
  strncpy(last_agent_provider, provider, sizeof(last_agent_provider) - 1);
  last_agent_provider[sizeof(last_agent_provider) - 1] = '\0';
  strncpy(last_agent_model, model, sizeof(last_agent_model) - 1);
  last_agent_model[sizeof(last_agent_model) - 1] = '\0';
  return 0;
}

static int l_agent_set_profile_info(lua_State *L) {
  const char *profile = luaL_optstring(L, 1, "");
  strncpy(last_agent_profile, profile, sizeof(last_agent_profile) - 1);
  last_agent_profile[sizeof(last_agent_profile) - 1] = '\0';
  return 0;
}

static int l_capstan_log(lua_State *L) {
  const char *category = luaL_checkstring(L, 1);
  const char *message = luaL_optstring(L, 2, "");
  size_t used = strlen(captured_logs);
  if (used < sizeof(captured_logs) - 1) {
    snprintf(captured_logs + used, sizeof(captured_logs) - used, "[%s] %s\n",
             category, message);
  }
  return 0;
}

static int l_state_path(lua_State *L) {
  const char *relative = luaL_optstring(L, 1, "state.lua");
  (void)relative;
  lua_pushstring(L, temp_state_path);
  return 1;
}

static int l_state_ensure_dir(lua_State *L) {
  lua_pushboolean(L, 1);
  return 1;
}

static lua_State *new_provider_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  luaL_dostring(L,
                "package.path = './?.lua;./?/init.lua;vendor/rxi/?.lua;' .. "
                "package.path");

  lua_newtable(L);
  lua_pushcfunction(L, l_http_get);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, l_http_post_stream);
  lua_setfield(L, -2, "post_stream");
  lua_pushcfunction(L, l_http_post_response_async);
  lua_setfield(L, -2, "post_response_async");
  lua_setglobal(L, "http");

  agent_init(L);
  lua_getglobal(L, "agent");
  lua_pushcfunction(L, l_agent_append);
  lua_setfield(L, -2, "append");
  lua_pushcfunction(L, l_agent_set_info);
  lua_setfield(L, -2, "set_info");
  lua_pushcfunction(L, l_agent_set_profile_info);
  lua_setfield(L, -2, "set_profile_info");
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "set_usage");
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "set_thinking");
  lua_setglobal(L, "agent");

  lua_newtable(L);
  lua_pushcfunction(L, l_permit_check);
  lua_setfield(L, -2, "check");
  lua_pushcfunction(L, l_permit_prompt);
  lua_setfield(L, -2, "prompt");
  lua_pushcfunction(L, l_permit_grant);
  lua_setfield(L, -2, "grant");
  lua_pushcfunction(L, l_permit_save);
  lua_setfield(L, -2, "save");
  lua_setglobal(L, "permit");

  lua_newtable(L);
  lua_pushcfunction(L, l_noop);
  lua_setfield(L, -2, "error");
  lua_setglobal(L, "popup");

  lua_newtable(L);
  lua_pushcfunction(L, l_capstan_log);
  lua_setfield(L, -2, "log");
  lua_pushcfunction(L, l_state_path);
  lua_setfield(L, -2, "state_path");
  lua_pushcfunction(L, l_state_ensure_dir);
  lua_setfield(L, -2, "state_ensure_dir");
  lua_setglobal(L, "capstan");

  luaL_dostring(L,
                "plugins = {"
                "fetch = {"
                "tool = {"
                "name = 'fetch',"
                "description = 'Fetch an HTTP or HTTPS URL',"
                "parameters = {"
                "type = 'object',"
                "properties = {url = {type = 'string'}},"
                "required = {'url'}"
                "}"
                "},"
                "handler = function(ctx) return ctx:replace('ui', 'llm') end"
                "},"
                "file = {"
                "tool = {"
                "name = 'file_read',"
                "description = 'Read a local file',"
                "parameters = {"
                "type = 'object',"
                "properties = {path = {type = 'string'}},"
                "required = {'path'}"
                "}"
                "},"
                "handler = function(ctx) return ctx:replace('file ui', 'file llm') end"
                "},"
                "shell = {"
                "tool = {"
                "name = 'shell',"
                "permission = 'shell',"
                "description = 'Execute a shell command',"
                "parameters = {"
                "type = 'object',"
                "properties = {command = {type = 'string'}},"
                "required = {'command'}"
                "}"
                "},"
                "handler = function(ctx) return ctx:replace('shell ui', 'shell llm: ' .. ctx.tool_args.command) end"
                "}"
                "}");

  return L;
}

static void reset_captures(lua_State *L) {
  if (stream_callback_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, stream_callback_ref);
    stream_callback_ref = LUA_NOREF;
  }
  captured_body[0] = '\0';
  captured_get_url[0] = '\0';
  captured_permit_tool[0] = '\0';
  captured_permit_target[0] = '\0';
  captured_logs[0] = '\0';
  captured_agent_appends[0] = '\0';
  last_agent_provider[0] = '\0';
  last_agent_model[0] = '\0';
  last_agent_profile[0] = '\0';
  snprintf(temp_state_path, sizeof(temp_state_path),
           "/tmp/capstan-provider-state-%ld.lua", (long)getpid());
  unlink(temp_state_path);
  permit_decision = "deny";
  permit_prompt_decision = "always";
  granted_tool[0] = '\0';
  granted_pattern[0] = '\0';
  grant_allow = 0;
  permit_prompt_calls = 0;
  permit_check_calls = 0;
  permit_save_calls = 0;
  mock_models_success = 0;
}

static void set_permit_decision(const char *decision) {
  permit_decision = decision;
}

static void set_permit_prompt_decision(const char *decision) {
  permit_prompt_decision = decision;
}

static void set_capstan_workdir(lua_State *L, const char *path) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_pushstring(L, path);
  lua_setfield(L, -2, "workdir");
  lua_setglobal(L, "capstan");
}

static void set_capstan_provider_config(lua_State *L) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_newtable(L);
  lua_pushstring(L, "openrouter");
  lua_setfield(L, -2, "provider");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "config/model");
  lua_setfield(L, -2, "model");
  lua_setfield(L, -2, "openrouter");
  lua_setfield(L, -2, "providers");
  lua_setfield(L, -2, "config");
  lua_setglobal(L, "capstan");
}

static void set_capstan_custom_provider_config(lua_State *L) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_newtable(L);
  lua_pushstring(L, "custom");
  lua_setfield(L, -2, "provider");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "https://llm.example/v1/chat/completions");
  lua_setfield(L, -2, "endpoint");
  lua_pushstring(L, "custom/default");
  lua_setfield(L, -2, "model");
  lua_setfield(L, -2, "custom");
  lua_setfield(L, -2, "providers");
  lua_setfield(L, -2, "config");
  lua_setglobal(L, "capstan");
}

static void set_subagents_capability(lua_State *L, int enabled) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "capabilities");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_pushboolean(L, enabled);
  lua_setfield(L, -2, "subagents");
  lua_setfield(L, -2, "capabilities");
  lua_setfield(L, -2, "config");
  lua_setglobal(L, "capstan");
}

static void set_capstan_state_model(lua_State *L, const char *provider,
                                    const char *model) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "state");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "models");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_pushstring(L, model);
  lua_setfield(L, -2, provider);
  lua_setfield(L, -2, "models");
  lua_setfield(L, -2, "state");
  lua_setglobal(L, "capstan");
}

static void set_capstan_state_provider(lua_State *L, const char *provider) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "state");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_pushstring(L, provider);
  lua_setfield(L, -2, "provider");
  lua_setfield(L, -2, "state");
  lua_setglobal(L, "capstan");
}

static void set_capstan_config_weak_model(lua_State *L, const char *provider,
                                          const char *model) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_newtable(L);
  lua_pushstring(L, provider);
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, model);
  lua_setfield(L, -2, "model");
  lua_setfield(L, -2, "weak_model");
  lua_setfield(L, -2, "config");
  lua_setglobal(L, "capstan");
}

static void set_capstan_state_weak_model(lua_State *L, const char *provider,
                                         const char *model) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "state");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_newtable(L);
  lua_pushstring(L, provider);
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, model);
  lua_setfield(L, -2, "model");
  lua_setfield(L, -2, "weak_model");
  lua_setfield(L, -2, "state");
  lua_setglobal(L, "capstan");
}

static void set_capstan_config_profile_model(lua_State *L, const char *profile,
                                             const char *provider,
                                             const char *model) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "agent");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "profile_models");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_newtable(L);
  lua_pushstring(L, provider);
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, model);
  lua_setfield(L, -2, "model");
  lua_setfield(L, -2, profile);
  lua_setfield(L, -2, "profile_models");
  lua_setfield(L, -2, "agent");
  lua_setfield(L, -2, "config");
  lua_setglobal(L, "capstan");
}

static void set_capstan_mcp_config(lua_State *L) {
  int rc = luaL_dostring(
      L,
      "capstan.config = capstan.config or {}\n"
      "capstan.config.mcp = {\n"
      "  enabled = true,\n"
      "  servers = {{name = 'stub', command = 'stub-mcp'}}\n"
      "}\n"
      "MCP_NOW_MS = 0\n"
      "capstan.now_ms = function() MCP_NOW_MS = MCP_NOW_MS + 10; return MCP_NOW_MS end\n"
      "MCP_SPAWN_CALLS = 0\n"
      "MCP_LAST_ID = 0\n"
      "MCP_LAST_METHOD = ''\n"
      "mcp = {\n"
      "  spawn = function(command, args, env)\n"
      "    MCP_SPAWN_CALLS = MCP_SPAWN_CALLS + 1\n"
      "    return 1\n"
      "  end,\n"
      "  send = function(handle, payload)\n"
      "    MCP_LAST_ID = tonumber(payload:match('\"id\":(%d+)')) or MCP_LAST_ID\n"
      "    MCP_LAST_METHOD = payload:match('\"method\":\"([^\"]+)\"') or ''\n"
      "    return true\n"
      "  end,\n"
      "  recv = function(handle, timeout)\n"
      "    if MCP_LAST_METHOD == 'initialize' then\n"
      "      return '{\"jsonrpc\":\"2.0\",\"id\":' .. MCP_LAST_ID .. ',\"result\":{\"serverInfo\":{\"name\":\"stub\",\"version\":\"1\"}}}'\n"
      "    end\n"
      "    return '{\"jsonrpc\":\"2.0\",\"id\":' .. MCP_LAST_ID .. ',\"result\":{\"tools\":[{\"name\":\"demo\",\"description\":\"Demo\"}]}}'\n"
      "  end,\n"
      "  recv_nowait = function(handle)\n"
      "    return mcp.recv(handle, 0)\n"
      "  end,\n"
      "  kill = function(handle) end,\n"
      "}\n");
  munit_assert_int(rc, ==, LUA_OK);
}

static void set_capstan_http_mcp_config(lua_State *L) {
  int rc = luaL_dostring(
      L,
      "capstan.config = capstan.config or {}\n"
      "capstan.config.mcp = {\n"
      "  enabled = true,\n"
      "  servers = {{name = 'github', transport = 'streamable_http', url = 'https://api.githubcopilot.com/mcp/'}}\n"
      "}\n");
  munit_assert_int(rc, ==, LUA_OK);
}

static void set_capstan_state_profile_model(lua_State *L, const char *profile,
                                            const char *provider,
                                            const char *model) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "state");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_getfield(L, -1, "profile_models");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_newtable(L);
  lua_pushstring(L, provider);
  lua_setfield(L, -2, "provider");
  lua_pushstring(L, model);
  lua_setfield(L, -2, "model");
  lua_setfield(L, -2, profile);
  lua_setfield(L, -2, "profile_models");
  lua_setfield(L, -2, "state");
  lua_setglobal(L, "capstan");
}

static void set_config_hook(lua_State *L, const char *hook_lua) {
  char script[4096];
  snprintf(script, sizeof(script),
           "capstan.config = capstan.config or {} "
           "capstan.config.hooks = {%s}",
           hook_lua);
  int rc = luaL_dostring(L, script);
  munit_assert_int(rc, ==, LUA_OK);
}

static void set_agent_config_number(lua_State *L, const char *field,
                                    int value) {
  char script[512];
  snprintf(script, sizeof(script),
           "capstan.config = capstan.config or {} "
           "capstan.config.agent = capstan.config.agent or {} "
           "capstan.config.agent.%s = %d",
           field, value);
  int rc = luaL_dostring(L, script);
  munit_assert_int(rc, ==, LUA_OK);
}

static void install_mock_now_ms(lua_State *L, int value) {
  char script[256];
  snprintf(script, sizeof(script),
           "MOCK_NOW_MS = %d "
           "capstan.now_ms = function() return MOCK_NOW_MS end",
           value);
  int rc = luaL_dostring(L, script);
  munit_assert_int(rc, ==, LUA_OK);
}

static void set_mock_now_ms(lua_State *L, int value) {
  char script[64];
  snprintf(script, sizeof(script), "MOCK_NOW_MS = %d", value);
  int rc = luaL_dostring(L, script);
  munit_assert_int(rc, ==, LUA_OK);
}

static void load_real_file_edit_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file_edit.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));

  lua_getglobal(L, "plugins");
  munit_assert_true(lua_istable(L, -1));
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "file_edit");
  lua_pop(L, 2);
}

static void load_real_file_write_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file_write.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));

  lua_getglobal(L, "plugins");
  munit_assert_true(lua_istable(L, -1));
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "file_write");
  lua_pop(L, 2);
}

static void load_real_file_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/file.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));

  lua_getglobal(L, "plugins");
  munit_assert_true(lua_istable(L, -1));
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "file");
  lua_pop(L, 2);
}

static void install_broken_tool_plugin(lua_State *L) {
  int rc = luaL_dostring(
      L,
      "plugins.broken = {"
      "tool = {"
      "name = 'broken_tool',"
      "permission = 'broken_tool',"
      "description = 'Broken test tool',"
      "parameters = {type = 'object', properties = {path = {type = 'string'}}, required = {'path'}}"
      "},"
      "handler = function(ctx) error('boom for ' .. tostring(ctx.tool_args.path)) end"
      "}");
  munit_assert_int(rc, ==, LUA_OK);
}

static void make_tmp_dir(char *buf, size_t buf_size, const char *name) {
  for (int i = 0; i < 1000; i++) {
    snprintf(buf, buf_size, "/tmp/capstan-%s-%ld-%d", name, (long)getpid(),
             i);
    if (mkdir(buf, 0700) == 0) {
      return;
    }
    if (errno != EEXIST) {
      break;
    }
  }
  munit_error("failed to create temp dir");
}

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "wb");
  munit_assert_not_null(f);
  munit_assert_size(fwrite(content, 1, strlen(content), f), ==,
                    strlen(content));
  fclose(f);
}

static void read_file(const char *path, char *buf, size_t buf_size) {
  FILE *f = fopen(path, "rb");
  munit_assert_not_null(f);
  size_t n = fread(buf, 1, buf_size - 1, f);
  fclose(f);
  buf[n] = '\0';
}

static void call_agent_entry(lua_State *L) {
  lua_getglobal(L, "agent_entry");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "Fetch https://example.com");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  int rc = lua_pcall(L, 1, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_agent_run_with_reasoning_effort(lua_State *L,
                                                 const char *effort) {
  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");

  lua_newtable(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "Fetch https://example.com");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "messages");
  lua_pushstring(L, effort);
  lua_setfield(L, -2, "reasoning_effort");

  lua_newtable(L);
  int rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 4);
}

static void call_agent_run_with_profile(lua_State *L, const char *profile) {
  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");

  lua_newtable(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "Inspect the project");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "messages");
  lua_pushstring(L, profile);
  lua_setfield(L, -2, "profile");

  lua_newtable(L);
  int rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 4);
}

static void call_agent_run_with_profile_and_model(lua_State *L,
                                                  const char *profile,
                                                  const char *model) {
  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");

  lua_newtable(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "Inspect the project");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "messages");
  lua_pushstring(L, profile);
  lua_setfield(L, -2, "profile");
  lua_pushstring(L, model);
  lua_setfield(L, -2, "model");

  lua_newtable(L);
  int rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 4);
}

static void send_tool_call(lua_State *L, const char *call_id,
                           const char *name, const char *arguments);
static void send_text_done(lua_State *L, const char *text);

static MunitResult test_request_enables_auto_tool_choice(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"tools\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"fetch\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"file_read\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"shell\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"subagents\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"tool_choice\":\"auto\"") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] request") != NULL);
  munit_assert_true(strstr(captured_logs, "depth=0 kind=orchestrator") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] tools=fetch,file_read,shell,subagents") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] last_message role=user") != NULL);
  munit_assert_true(strstr(captured_logs, "[api] post_stream") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_request_applies_reasoning_effort(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_run_with_reasoning_effort(L, "low");

  munit_assert_true(strstr(captured_body, "\"reasoning\":{\"effort\":\"low\"}") != NULL);
  munit_assert_true(strstr(captured_body, "Reasoning effort: low") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_config_applies_reasoning_effort(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "capstan = capstan or {}\n"
      "capstan.config = {agent = {reasoning_effort = 'minimal'}}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"reasoning\":{\"effort\":\"minimal\"}") != NULL);
  munit_assert_true(strstr(captured_body, "Reasoning effort: minimal") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_agent_reasoning_effort_accessor(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "capstan = capstan or {}\n"
      "capstan.config = {agent = {reasoning_effort = 'minimal'}}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "reasoning_effort");
  lua_pushstring(L, "fast");
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(lua_tostring(L, -1), "minimal");
  lua_pop(L, 3);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_default_profile_is_implement(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  munit_assert_string_equal(last_agent_profile, "implement");

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "Active Profile: Implement") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning\":{\"effort\":\"medium\"}") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] profile=implement") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_mcp_initializes_lazily_after_startup(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_mcp_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "MCP_SPAWN_CALLS");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 0);
  lua_pop(L, 1);

  call_agent_entry(L);

  lua_getglobal(L, "MCP_SPAWN_CALLS");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  munit_assert_true(strstr(captured_body, "\"name\":\"mcp__stub__demo\"") == NULL);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "mcp");
  lua_getfield(L, -1, "tick");
  lua_pushinteger(L, 2);
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 3);

  reset_captures(L);
  call_agent_entry(L);
  munit_assert_true(strstr(captured_body, "\"name\":\"mcp__stub__demo\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_http_mcp_initializes_without_stdio_spawn(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_http_mcp_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  lua_getglobal(L, "MCP_SPAWN_CALLS");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);
  munit_assert_true(strstr(captured_body, "\"name\":\"mcp__github__search_repositories\"") == NULL);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "mcp");
  lua_getfield(L, -1, "tick");
  lua_pushinteger(L, 2);
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 3);

  reset_captures(L);
  call_agent_entry(L);
  munit_assert_true(strstr(captured_body, "\"name\":\"mcp__github__search_repositories\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_disable_mcp_prevents_background_tick(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_mcp_config(L);

  int rc = luaL_dostring(
      L,
      "capstan.runtime_options = {disable_mcp = true}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "mcp");
  lua_getfield(L, -1, "tick");
  lua_pushinteger(L, 3);
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 3);

  lua_getglobal(L, "MCP_SPAWN_CALLS");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 0);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_plan_profile_filters_tools_and_prompt(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_run_with_profile(L, "plan");

  munit_assert_true(strstr(captured_body, "Active Profile: Plan") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning\":{\"effort\":\"high\"}") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"fetch\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"file_read\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"shell\"") == NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"subagents\"") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] tools=fetch,file_read,subagents") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] profile=plan") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_plan_profile_rejects_unavailable_tool_call(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_run_with_profile(L, "plan");
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_forbidden_write", "file_write",
                 "{\\\"path\\\":\\\"out.txt\\\",\\\"content\\\":\\\"nope\\\"}");

  munit_assert_int(permit_check_calls, ==, 0);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_string_equal(captured_permit_tool, "");
  munit_assert_true(strstr(captured_logs,
                           "[tool] unavailable name=file_write") != NULL);
  munit_assert_true(strstr(captured_agent_appends,
                           "file_write: unavailable") != NULL);
  munit_assert_true(strstr(captured_body,
                           "Tool file_write is not available in the active profile") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_fast_profile_applies_reasoning_and_prompt(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_run_with_profile(L, "fast");

  munit_assert_true(strstr(captured_body, "Active Profile: Fast") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning\":{\"effort\":\"low\"}") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"shell\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"subagents\"") != NULL);
  munit_assert_true(strstr(captured_logs, "[agent] profile=fast") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_tool_enabled_by_default(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"name\":\"subagents\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_tool_disabled_by_capability(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_subagents_capability(L, 0);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"name\":\"subagents\"") == NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] tools=fetch,file_read,shell") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_tool_returns_structured_results(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagents_tool_result = ''\n"
      "subagents_tool_lists = {}\n"
      "subagents_tool_turn_limits = {}\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  local names = {}\n"
      "  for _, tool in ipairs(opts.tools or {}) do table.insert(names, tool['function'].name) end\n"
      "  table.sort(names)\n"
      "  table.insert(subagents_tool_lists, table.concat(names, ','))\n"
      "  table.insert(subagents_tool_turn_limits, opts.max_turns)\n"
      "  callbacks.on_done({ok = true, text = 'done:' .. opts.messages[1].content, turns = 2, started_at = 1000, finished_at = 1500, duration_ms = 500})\n"
      "  return true, nil\n"
      "end\n"
      "local current_msgs = {}\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls(current_msgs, available, {{id='call_subs', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"docs\\\",\\\"task\\\":\\\"read docs\\\",\\\"tools\\\":[\\\"fetch\\\"],\\\"max_turns\\\":250},{\\\"id\\\":\\\"build\\\",\\\"task\\\":\\\"check build\\\"}],\\\"max_concurrent\\\":2}'}}, '', function(msgs)\n"
      "  subagents_tool_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagents_tool_result");
  const char *result = lua_tostring(L, -1);
  munit_assert_not_null(result);
  munit_assert_true(strstr(result, "\"duration_ms\"") != NULL);
  munit_assert_true(strstr(result, "\"total_turns\":4") != NULL);
  munit_assert_true(strstr(result, "\"id\":\"docs\"") != NULL);
  munit_assert_true(strstr(result, "\"id\":\"build\"") != NULL);
  munit_assert_true(strstr(result, "\"turns\":2") != NULL);
  lua_pop(L, 1);

  lua_getglobal(L, "subagents_tool_lists");
  lua_rawgeti(L, -1, 1);
  munit_assert_string_equal(lua_tostring(L, -1), "fetch");
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  const char *second_tools = lua_tostring(L, -1);
  munit_assert_not_null(second_tools);
  munit_assert_true(strstr(second_tools, "fetch") != NULL);
  munit_assert_true(strstr(second_tools, "file_read") != NULL);
  munit_assert_true(strstr(second_tools, "shell") != NULL);
  munit_assert_true(strstr(second_tools, "subagents") == NULL);
  lua_pop(L, 2);

  lua_getglobal(L, "subagents_tool_turn_limits");
  lua_rawgeti(L, -1, 1);
  munit_assert_int((int)lua_tointeger(L, -1), ==, 200);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  munit_assert_int((int)lua_tointeger(L, -1), ==, 6);
  lua_pop(L, 2);

  munit_assert_string_equal(captured_permit_tool, "");
  munit_assert_string_equal(captured_permit_target, "");
  munit_assert_true(strstr(captured_agent_appends, "subagents: running 2 concurrent, 2 total") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "subagents: done 2/2") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_plan_subagents_inherit_profile_and_readonly_tools(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local profiles = require('agent.profiles')\n"
      "subagents_child_profiles = {}\n"
      "subagents_child_tools = {}\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  table.insert(subagents_child_profiles, opts.profile or '')\n"
      "  local names = {}\n"
      "  for _, tool in ipairs(opts.tools or {}) do table.insert(names, tool['function'].name) end\n"
      "  table.sort(names)\n"
      "  table.insert(subagents_child_tools, table.concat(names, ','))\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = profiles.filter_tools(tools.collect(), profiles.get('plan'))\n"
      "tools.handle_tool_calls({}, available, {{id='call_plan_subs', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"a\\\",\\\"task\\\":\\\"inspect a\\\"},{\\\"id\\\":\\\"b\\\",\\\"task\\\":\\\"inspect b\\\",\\\"tools\\\":[\\\"fetch\\\",\\\"shell\\\"]}],\\\"max_concurrent\\\":2}'}}, '', function() end, {tools = available, depth = 0, profile = 'plan'})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagents_child_profiles");
  lua_rawgeti(L, -1, 1);
  munit_assert_string_equal(lua_tostring(L, -1), "plan");
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  munit_assert_string_equal(lua_tostring(L, -1), "plan");
  lua_pop(L, 2);

  lua_getglobal(L, "subagents_child_tools");
  lua_rawgeti(L, -1, 1);
  munit_assert_string_equal(lua_tostring(L, -1), "fetch,file_read");
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  munit_assert_string_equal(lua_tostring(L, -1), "fetch");
  lua_pop(L, 2);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_respect_explicit_small_max_turns(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagent_turn_limit = 0\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  subagent_turn_limit = opts.max_turns\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_min_turns', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"one\\\",\\\"task\\\":\\\"fetch one known URL\\\",\\\"max_turns\\\":2}]}' }}, '', function() end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_turn_limit");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 2);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_pass_shared_instructions_to_children(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagent_prompt = ''\n"
      "subagent_tool_list = ''\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  subagent_prompt = opts.messages[1].content\n"
      "  local names = {}\n"
      "  for _, tool in ipairs(opts.tools or {}) do table.insert(names, tool['function'].name) end\n"
      "  table.sort(names)\n"
      "  subagent_tool_list = table.concat(names, ',')\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_instructions', name='subagents', arguments='{\\\"instructions\\\":\\\"Use the already loaded web-search skill: call curl with the Brave endpoint and X-Subscription-Token from BRAVE_SEARCH_API_KEY. Do not use browser MCP.\\\",\\\"tasks\\\":[{\\\"id\\\":\\\"standards\\\",\\\"instructions\\\":\\\"Search only authoritative sources.\\\",\\\"task\\\":\\\"Research C coding standards\\\",\\\"tools\\\":[\\\"shell\\\"],\\\"max_turns\\\":3}]}' }}, '', function() end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_prompt");
  const char *prompt = lua_tostring(L, -1);
  munit_assert_not_null(prompt);
  munit_assert_true(strstr(prompt, "Shared instructions from orchestrator:") != NULL);
  munit_assert_true(strstr(prompt, "already loaded web-search skill") != NULL);
  munit_assert_true(strstr(prompt, "Do not use browser MCP") != NULL);
  munit_assert_true(strstr(prompt, "Task-specific instructions:") != NULL);
  munit_assert_true(strstr(prompt, "Search only authoritative sources") != NULL);
  munit_assert_true(strstr(prompt, "Task:\nResearch C coding standards") != NULL);
  lua_pop(L, 1);

  lua_getglobal(L, "subagent_tool_list");
  munit_assert_string_equal(lua_tostring(L, -1), "shell");
  lua_pop(L, 1);

  munit_assert_true(strstr(captured_logs,
                           "[subagents] child index=1 id=standards depth=1 max_turns=3 tools=1 tool_names=shell") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_wait_loop_yields_between_polls(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagent_callbacks = {}\n"
      "subagent_wait_frames = 0\n"
      "subagent_result = ''\n"
      "http.poll_index = 0\n"
      "http.poll = function()\n"
      "  http.poll_index = http.poll_index + 1\n"
      "  local cb = subagent_callbacks[http.poll_index]\n"
      "  if cb then cb.on_done({ok = true, text = 'ok', turns = 1}) end\n"
      "  return cb and 1 or 0\n"
      "end\n"
      "http.wait_frame = function() subagent_wait_frames = subagent_wait_frames + 1 end\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  table.insert(subagent_callbacks, callbacks)\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_wait', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"one\\\",\\\"task\\\":\\\"one\\\"},{\\\"id\\\":\\\"two\\\",\\\"task\\\":\\\"two\\\"}],\\\"max_concurrent\\\":1}'}}, '', function(msgs)\n"
      "  subagent_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_wait_frames");
  munit_assert_int((int)lua_tointeger(L, -1), >=, 2);
  lua_pop(L, 1);

  lua_getglobal(L, "subagent_result");
  const char *result = lua_tostring(L, -1);
  munit_assert_not_null(result);
  munit_assert_true(strstr(result, "\"id\":\"one\"") != NULL);
  munit_assert_true(strstr(result, "\"id\":\"two\"") != NULL);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_retry_transient_http_errors(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagent_attempts = 0\n"
      "subagent_result = ''\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  subagent_attempts = subagent_attempts + 1\n"
      "  if subagent_attempts < 3 then\n"
      "    callbacks.on_done({ok = false, error = 'HTTP 429', text = '', turns = 1})\n"
      "  else\n"
      "    callbacks.on_done({ok = true, text = 'ok', turns = 1})\n"
      "  end\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_retry', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"limited\\\",\\\"task\\\":\\\"limited\\\"}]}' }}, '', function(msgs)\n"
      "  subagent_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_attempts");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 3);
  lua_pop(L, 1);

  lua_getglobal(L, "subagent_result");
  const char *result = lua_tostring(L, -1);
  munit_assert_not_null(result);
  munit_assert_true(strstr(result, "\"ok\":true") != NULL);
  munit_assert_true(strstr(result, "\"attempts\":3") != NULL);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_do_not_retry_http_400(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagent_attempts = 0\n"
      "subagent_result = ''\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  subagent_attempts = subagent_attempts + 1\n"
      "  callbacks.on_done({ok = false, error = 'HTTP 400', text = '', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_400', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"bad\\\",\\\"task\\\":\\\"bad\\\"}]}' }}, '', function(msgs)\n"
      "  subagent_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_attempts");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);

  lua_getglobal(L, "subagent_result");
  const char *result = lua_tostring(L, -1);
  munit_assert_not_null(result);
  munit_assert_true(strstr(result, "\"ok\":false") != NULL);
  munit_assert_true(strstr(result, "\"error\":\"HTTP 400\"") != NULL);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_use_current_provider_models_only(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  mock_models_success = 1;

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "capstan");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "agent_runtime");
  lua_pop(L, 2);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "subagents_providers = {}\n"
      "subagents_models = {}\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  table.insert(subagents_providers, opts.provider or '')\n"
      "  table.insert(subagents_models, opts.model or '')\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local current_msgs = {}\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls(current_msgs, available, {{id='call_subs_models', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"valid\\\",\\\"task\\\":\\\"valid\\\",\\\"provider\\\":\\\"deepseek\\\",\\\"model\\\":\\\"model/a\\\"},{\\\"id\\\":\\\"invalid\\\",\\\"task\\\":\\\"invalid\\\",\\\"provider\\\":\\\"deepseek\\\",\\\"model\\\":\\\"gpt-4.1\\\"}]}' }}, '', function() end, {runtime = capstan.agent_runtime, provider = capstan.agent_runtime.providers.openrouter, provider_name = 'openrouter', tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagents_providers");
  lua_rawgeti(L, -1, 1);
  munit_assert_string_equal(lua_tostring(L, -1), "openrouter");
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  munit_assert_string_equal(lua_tostring(L, -1), "openrouter");
  lua_pop(L, 2);

  lua_getglobal(L, "subagents_models");
  lua_rawgeti(L, -1, 1);
  munit_assert_string_equal(lua_tostring(L, -1), "model/a");
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  munit_assert_string_equal(lua_tostring(L, -1), "config/model");
  lua_pop(L, 2);

  munit_assert_true(strstr(captured_get_url, "/models") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "ignored unavailable model=gpt-4.1") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[subagents] start index=1 id=valid attempt=1/3 provider=openrouter model=model/a prompt=Task: valid") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[subagents] start index=2 id=invalid attempt=1/3 provider=openrouter model=config/model prompt=Task: invalid") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_config_sets_default_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"config/model\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_state_model_overrides_config_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "state/model");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"state/model\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_config_profile_model_selected_for_profile(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_config_profile_model(L, "plan", "openrouter", "config/plan");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_run_with_profile(L, "plan");

  munit_assert_true(strstr(captured_body, "\"model\":\"config/plan\"") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] request provider=openrouter model=config/plan") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_runtime_startup_publishes_configured_profile_status(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_config_profile_model(L, "plan", "openrouter", "config/plan");
  int rc = luaL_dostring(
      L,
      "capstan.config.agent = capstan.config.agent or {}\n"
      "capstan.config.agent.profile = 'plan'\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  munit_assert_string_equal(last_agent_provider, "openrouter");
  munit_assert_string_equal(last_agent_model, "config/plan");
  munit_assert_string_equal(last_agent_profile, "plan");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_state_profile_model_overrides_config_profile_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_config_profile_model(L, "plan", "openrouter", "config/plan");
  set_capstan_state_profile_model(L, "plan", "openrouter", "state/plan");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_run_with_profile(L, "plan");

  munit_assert_true(strstr(captured_body, "\"model\":\"state/plan\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_explicit_model_overrides_profile_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_config_profile_model(L, "plan", "openrouter", "config/plan");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_run_with_profile_and_model(L, "plan", "explicit/model");

  munit_assert_true(strstr(captured_body, "\"model\":\"explicit/model\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"model\":\"config/plan\"") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_state_provider_overrides_config_provider(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_state_provider(L, "deepseek");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"deepseek-chat\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_env_provider_overrides_state_provider(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  setenv("AI_PROVIDER", "openrouter", 1);
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_state_provider(L, "deepseek");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"config/model\"") != NULL);

  unsetenv("AI_PROVIDER");
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_env_model_overrides_state_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  setenv("OPENROUTER_MODEL", "env/model", 1);
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "state/model");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"env/model\"") != NULL);

  unsetenv("OPENROUTER_MODEL");
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_set_for_persists_active_provider(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "set_for");
  lua_pushstring(L, "deepseek");
  lua_pushstring(L, "deepseek-chat");
  rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));

  lua_getfield(L, -3, "current_provider");
  rc = lua_pcall(L, 0, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(lua_tostring(L, -1), "deepseek");

  char contents[768];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "provider = \"deepseek\"") != NULL);
  munit_assert_true(strstr(contents, "[\"deepseek\"] = \"deepseek-chat\"") != NULL);

  unlink(temp_state_path);
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_set_persists_state_file(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "set");
  lua_pushstring(L, "persisted/model");
  rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));

  char contents[512];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "[\"openrouter\"] = \"persisted/model\"") != NULL);

  unlink(temp_state_path);
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_config_sets_weak_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_config_weak_model(L, "deepseek", "deepseek-chat");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "weak");
  rc = lua_pcall(L, 0, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_getfield(L, -1, "provider");
  lua_getfield(L, -2, "model");
  munit_assert_string_equal(lua_tostring(L, -2), "deepseek");
  munit_assert_string_equal(lua_tostring(L, -1), "deepseek-chat");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_state_weak_model_overrides_config(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_config_weak_model(L, "deepseek", "config/weak");
  set_capstan_state_weak_model(L, "openrouter", "state/weak");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "weak");
  rc = lua_pcall(L, 0, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_getfield(L, -1, "provider");
  lua_getfield(L, -2, "model");
  munit_assert_string_equal(lua_tostring(L, -2), "openrouter");
  munit_assert_string_equal(lua_tostring(L, -1), "state/weak");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_set_weak_persists_state_file(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "set_weak");
  lua_pushstring(L, "openrouter");
  lua_pushstring(L, "persisted/weak");
  rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));

  char contents[768];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "weak_model = {") != NULL);
  munit_assert_true(strstr(contents, "provider = \"openrouter\"") != NULL);
  munit_assert_true(strstr(contents, "model = \"persisted/weak\"") != NULL);

  unlink(temp_state_path);
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_set_profile_persists_state_file(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "set_profile");
  lua_pushstring(L, "plan");
  lua_pushstring(L, "openrouter");
  lua_pushstring(L, "persisted/plan");
  rc = lua_pcall(L, 3, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));

  char contents[1024];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "profile_models = {") != NULL);
  munit_assert_true(strstr(contents, "[\"plan\"] = {") != NULL);
  munit_assert_true(strstr(contents, "provider = \"openrouter\"") != NULL);
  munit_assert_true(strstr(contents, "model = \"persisted/plan\"") != NULL);

  unlink(temp_state_path);
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_compact_uses_weak_model_and_replaces_history(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  clear_messages();
  set_capstan_provider_config(L);
  set_capstan_config_weak_model(L, "openrouter", "weak/model");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  char *user = malloc(strlen("please implement compact") + 1);
  char *assistant = malloc(strlen("I changed files") + 1);
  munit_assert_not_null(user);
  munit_assert_not_null(assistant);
  strcpy(user, "please implement compact");
  strcpy(assistant, "I changed files");
  add_message(user, user, MSG_USER);
  add_message(assistant, assistant, MSG_AGENT);

  agent_compact(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  munit_assert_true(strstr(captured_body, "\"model\":\"weak/model\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"tools\"") == NULL);
  munit_assert_true(strstr(captured_body, "operational handoff summary") != NULL);

  send_text_done(L, "Goal: continue compact work");

  Messages *msgs = get_messages();
  munit_assert_size(msgs->size, ==, 1);
  munit_assert_int(msgs->items[0]->role, ==, MSG_USER);
  munit_assert_true(strstr(msgs->items[0]->text, "[context compacted]") != NULL);
  munit_assert_true(strstr(msgs->items[0]->text,
                           "Goal: continue compact work") != NULL);
  munit_assert_true(strstr(msgs->items[0]->raw_text,
                           "Previous conversation was compacted") != NULL);

  clear_messages();
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_list_uses_api_response(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  mock_models_success = 1;

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "list");
  rc = lua_pcall(L, 0, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -2));
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);

  munit_assert_int((int)lua_rawlen(L, -1), ==, 2);
  lua_rawgeti(L, -1, 1);
  lua_getfield(L, -1, "id");
  lua_getfield(L, -2, "text");
  munit_assert_string_equal(lua_tostring(L, -2), "model/a");
  munit_assert_string_equal(lua_tostring(L, -1), "model/a  Model A");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_list_all_includes_static_config_models(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  int config_rc = luaL_dostring(
      L,
      "capstan.config = {providers = {static_provider = {"
      "endpoint = 'https://llm.example/v1/chat/completions',"
      "model = 'static/model-a',"
      "models = {{id = 'static/model-a', context_limit = 1234}}"
      "}}}\n");
  munit_assert_int(config_rc, ==, LUA_OK);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "list_all");
  rc = lua_pcall(L, 0, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);

  int found = 0;
  for (int i = 1; i <= (int)lua_rawlen(L, -1); i++) {
    lua_rawgeti(L, -1, i);
    lua_getfield(L, -1, "provider");
    lua_getfield(L, -2, "id");
    const char *provider = lua_tostring(L, -2);
    const char *id = lua_tostring(L, -1);
    if (provider && id && strcmp(provider, "static_provider") == 0 &&
        strcmp(id, "static/model-a") == 0)
      found = 1;
    lua_pop(L, 3);
  }
  munit_assert_true(found);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_endpoint_derived_from_chat_endpoint(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_custom_provider_config(L);
  mock_models_success = 1;

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "models");
  lua_getfield(L, -1, "list");
  rc = lua_pcall(L, 0, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -2));
  munit_assert_true(lua_isnil(L, -1));
  munit_assert_string_equal(captured_get_url, "https://llm.example/v1/models");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_fetch_permission_target_uses_url(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                 "\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_1\",\"function\":{\"name\":\"fetch\","
                 "\"arguments\":\"{\\\"url\\\":\\\"https://example.com\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "fetch");
  munit_assert_string_equal(captured_permit_target, "https://example.com");
  munit_assert_true(strstr(captured_logs, "[tools] received 1 tool call") != NULL);
  munit_assert_true(strstr(captured_logs, "[tool] call name=fetch") != NULL);
  munit_assert_true(strstr(captured_logs, "[permit] tool=fetch") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_logs_text_when_model_does_not_call_tool(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"I will look at it.\"}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_true(strstr(captured_logs, "[stream] done") != NULL);
  munit_assert_true(strstr(captured_logs, "final_tool_calls=0") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] stream done without tool calls text=I will look at it.") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_stream_error_finishes_with_error(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  lua_pushstring(L, "HTTP 401");
  lua_pushstring(L, "{\"error\":\"bad key\"}");
  rc = lua_pcall(L, 4, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_true(strstr(captured_logs, "[agent] stream failed error=HTTP 401") != NULL);
  munit_assert_true(strstr(captured_logs, "bad key") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_file_read_permission_target_uses_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                 "\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_2\",\"function\":{\"name\":\"file_read\","
                 "\"arguments\":\"{\\\"path\\\":\\\"README\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target, "README");
  munit_assert_true(strstr(captured_logs, "[tool] call name=file_read") != NULL);
  munit_assert_true(strstr(captured_logs, "[permit] tool=file_read") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_sensitive_file_read_forces_prompt(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_permit_prompt_decision("deny");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                 "\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_env\",\"function\":{\"name\":\"file_read\","
                 "\"arguments\":\"{\\\"path\\\":\\\".env\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target, "/repo/project/.env");
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_true(strstr(captured_body, "User denied file_read") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_chunked_tool_call_arguments_continue_with_tool_result(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_chunked\",\"function\":{\"name\":\"fetch\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"function\":{\"arguments\":\"{\\\"url\\\"\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"function\":{\"arguments\":\":\\\"https://example.com\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "fetch");
  munit_assert_string_equal(captured_permit_target, "https://example.com");
  munit_assert_true(strstr(captured_logs, "[stream] tool_delta") == NULL);
  munit_assert_true(strstr(captured_logs, "[stream] tool_final") != NULL);
  munit_assert_true(strstr(captured_logs, "final_tool_calls=1") != NULL);
  munit_assert_true(strstr(captured_logs, "[tools] continuing with tool results") != NULL);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"tool_call_id\":\"call_chunked\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_stream_tool_delta_logs_only_at_debug(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  munit_assert_int(setenv("LOG_LEVEL", "debug", 1), ==, 0);

  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_debug\",\"function\":{\"name\":\"fetch\","
                 "\"arguments\":\"{\\\"url\\\":\\\"https://example.com\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_true(strstr(captured_logs, "[stream] tool_delta") != NULL);

  reset_captures(L);
  lua_close(L);
  unsetenv("LOG_LEVEL");
  return MUNIT_OK;
}

static MunitResult test_streamed_file_edit_tool_edits_file(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-file-edit");
  char path[4096];
  snprintf(path, sizeof(path), "%s/file.txt", workdir);
  write_file(path, "alpha\nbeta\ngamma\n");

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_edit_plugin(L);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_edit\",\"function\":{\"name\":\"file_edit\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"function\":{\"arguments\":\"{\\\"path\\\":\\\"file.txt\\\",\\\"old_text\\\":\\\"beta\\\"\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"function\":{\"arguments\":\",\\\"new_text\\\":\\\"BETA\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  char buf[128];
  read_file(path, buf, sizeof(buf));
  munit_assert_string_equal(buf, "alpha\nBETA\ngamma\n");
  munit_assert_string_equal(captured_permit_tool, "file_write");
  munit_assert_string_equal(captured_permit_target, path);
  munit_assert_true(strstr(captured_logs, "[tool] call name=file_edit") != NULL);
  munit_assert_true(strstr(captured_logs, "[permit] tool=file_write call=file_edit") != NULL);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "Edited ") != NULL);
  munit_assert_true(strstr(captured_body, "--- a/file.txt") != NULL);
  munit_assert_true(strstr(captured_body, "-beta") != NULL);
  munit_assert_true(strstr(captured_body, "+BETA") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "--- a/file.txt") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "-beta") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "+BETA") != NULL);

  reset_captures(L);
  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_file_read_tool_uses_tool_args_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-file-read-tool");
  char path[4096];
  snprintf(path, sizeof(path), "%s/NOTE.md", workdir);
  write_file(path, "tool args content\n");

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_plugin(L);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_file_read", "file_read",
                 "{\\\"path\\\":\\\"NOTE.md\\\"}");

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target, path);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "tool args content") != NULL);
  munit_assert_true(strstr(captured_body, "Usage: /file") == NULL);

  reset_captures(L);
  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_file_read_tool_allows_permitted_outside_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  char outside[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-file-read-work");
  make_tmp_dir(outside, sizeof(outside), "provider-file-read-out");
  char path[4096];
  snprintf(path, sizeof(path), "%s/NOTE.md", outside);
  write_file(path, "outside permitted content\n");

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_plugin(L);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  char args[8192];
  snprintf(args, sizeof(args), "{\\\"path\\\":\\\"%s\\\"}", path);
  send_tool_call(L, "call_file_read_outside", "file_read", args);

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target, path);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "outside permitted content") != NULL);
  munit_assert_true(strstr(captured_body, "escapes workspace") == NULL);

  reset_captures(L);
  lua_close(L);
  unlink(path);
  rmdir(outside);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_file_write_permission_target_uses_absolute_workspace_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-file-write-target");

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_write_plugin(L);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_write_target", "file_write",
                 "{\\\"path\\\":\\\"src/../src-plugins/out.txt\\\","
                 "\\\"content\\\":\\\"ok\\\"}");

  char expected_target[4096];
  snprintf(expected_target, sizeof(expected_target), "%s/src-plugins/out.txt",
           workdir);
  munit_assert_string_equal(captured_permit_tool, "file_write");
  munit_assert_string_equal(captured_permit_target, expected_target);
  munit_assert_true(strstr(captured_logs, "[permit] tool=file_write call=file_write") != NULL);

  char expected_file[4096];
  snprintf(expected_file, sizeof(expected_file), "%s/src-plugins/out.txt",
           workdir);
  char buf[32];
  read_file(expected_file, buf, sizeof(buf));
  munit_assert_string_equal(buf, "ok");

  reset_captures(L);
  lua_close(L);
  unlink(expected_file);
  char dir[4096];
  snprintf(dir, sizeof(dir), "%s/src-plugins", workdir);
  rmdir(dir);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_invalid_tool_arguments_skip_permission(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_bad\",\"function\":{\"name\":\"fetch\","
                 "\"arguments\":\"{bad json\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "");
  munit_assert_true(strstr(captured_logs, "[tool] invalid_args name=fetch") != NULL);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "Invalid JSON arguments") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_handler_error_returns_tool_result(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  install_broken_tool_plugin(L);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"call_broken\",\"function\":{\"name\":\"broken_tool\","
                 "\"arguments\":\"{\\\"path\\\":\\\"bad.txt\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "broken_tool");
  munit_assert_string_equal(captured_permit_target, "bad.txt");
  munit_assert_true(strstr(captured_agent_appends,
                           "error: Tool broken_tool failed") != NULL);
  munit_assert_true(strstr(captured_logs, "[tool] error name=broken_tool") != NULL);
  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "Tool broken_tool failed") != NULL);
  munit_assert_true(strstr(captured_body, "plugin: broken") != NULL);
  munit_assert_true(strstr(captured_body, "source: unknown") != NULL);
  munit_assert_true(strstr(captured_body, "args: {\\\"path\\\":\\\"bad.txt\\\"}") != NULL);
  munit_assert_true(strstr(captured_body, "traceback:") != NULL);
  munit_assert_true(strstr(captured_body, "boom for bad.txt") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static void send_tool_call(lua_State *L, const char *call_id,
                           const char *name, const char *arguments) {
  char event[2048];
  snprintf(event, sizeof(event),
           "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"%s\",\"function\":{\"name\":\"%s\","
           "\"arguments\":\"%s\"}}]}}]}\n\n",
           call_id, name, arguments);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, event);
  lua_pushboolean(L, 0);
  int rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  if (rc != LUA_OK) {
    munit_errorf("stream done callback failed: %s", lua_tostring(L, -1));
  }
}

static void send_text_done(lua_State *L, const char *text) {
  char event[2048];
  snprintf(event, sizeof(event),
           "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n",
           text);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, event);
  lua_pushboolean(L, 0);
  int rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static MunitResult test_tool_guard_stops_repeated_shell_command(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_same_shell_command", 2);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_guard_1", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_shell_guard_2", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  int permit_calls_before_stop = permit_check_calls;
  send_tool_call(L, "call_shell_guard_3", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_int(permit_check_calls, ==, permit_calls_before_stop);
  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: repeated shell command") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tool_guard] repeated shell command") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_allows_repeated_shell_by_default(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_default_1", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_shell_default_2", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_shell_default_3", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_int(permit_check_calls, ==, 3);
  munit_assert_true(strstr(captured_agent_appends, "[stopped:") == NULL);
  munit_assert_true(strstr(captured_logs, "[tool_guard]") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_shell_repeat_resets_after_other_tool(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_same_shell_command", 1);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_reset_1", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_fetch_reset", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  send_tool_call(L, "call_shell_reset_2", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_int(permit_check_calls, ==, 3);
  munit_assert_true(strstr(captured_agent_appends, "[stopped:") == NULL);
  munit_assert_true(strstr(captured_logs, "[tool_guard]") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_generic_repeat_resets_after_shell_tool(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_same_tool_call", 1);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_fetch_generic_reset_1", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  send_tool_call(L, "call_shell_generic_reset", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_fetch_generic_reset_2", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_int(permit_check_calls, ==, 3);
  munit_assert_true(strstr(captured_agent_appends, "[stopped:") == NULL);
  munit_assert_true(strstr(captured_logs, "[tool_guard]") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_stops_repeated_tool_call(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_same_tool_call", 1);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_fetch_guard_1", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  int permit_calls_before_stop = permit_check_calls;
  send_tool_call(L, "call_fetch_guard_2", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_int(permit_check_calls, ==, permit_calls_before_stop);
  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: repeated tool call fetch") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tool_guard] repeated tool call fetch") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_stops_total_tool_call_budget(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_tool_calls", 1);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_fetch_budget_1", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  int permit_calls_before_stop = permit_check_calls;
  send_tool_call(L, "call_file_budget_2", "file_read",
                 "{\\\"path\\\":\\\"README\\\"}");

  munit_assert_int(permit_check_calls, ==, permit_calls_before_stop);
  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: too many tool calls") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tool_guard] too many tool calls") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_stops_duration_before_tool_execution(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_duration_sec", 1);
  install_mock_now_ms(L, 0);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  set_mock_now_ms(L, 2000);
  send_tool_call(L, "call_fetch_duration", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_int(permit_check_calls, ==, 0);
  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: agent run exceeded 1s") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tool_guard] agent run exceeded 1s") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_stops_max_turns_before_continuation_request(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_turns", 1);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_fetch_max_turns", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: max agent turns exceeded: 1") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tool_guard] max agent turns exceeded: 1") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_always_allow_uses_workspace_target(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_1", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_string_equal(captured_permit_tool, "shell");
  munit_assert_string_equal(captured_permit_target, "/repo/project");
  munit_assert_string_equal(granted_tool, "shell");
  munit_assert_string_equal(granted_pattern, "/repo/project");
  munit_assert_int(grant_allow, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_true(strstr(captured_agent_appends, "⚙ shell") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "  $ pwd") != NULL);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_2", "shell",
                 "{\\\"command\\\":\\\"ls src\\\"}");

  munit_assert_string_equal(captured_permit_tool, "shell");
  munit_assert_string_equal(captured_permit_target, "/repo/project");
  munit_assert_int(permit_check_calls, ==, 2);
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_true(strstr(captured_body, "shell llm: ls src") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_tool_redacts_command_before_continuation(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/repo/project");
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "minimax/minimax-m3");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_secret", "shell",
                 "{\\\"command\\\":\\\"curl -H 'Authorization: Bearer secret-token' https://example.test\\\"}");

  munit_assert_true(strstr(captured_agent_appends, "secret-token") == NULL);
  munit_assert_true(strstr(captured_logs, "secret-token") == NULL);
  munit_assert_true(strstr(captured_body, "secret-token") == NULL);
  munit_assert_true(strstr(captured_body, "curl") != NULL);
  munit_assert_true(strstr(captured_body, "https://example.test") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_arguments_strip_minimax_markup(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/repo/project");
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "minimax/minimax-m3");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_minimax_markup", "shell",
                 "{\\\"command\\\":\\\"pwd ]<]minimax[>[</command>]\\\"}]<]minimax[>[</tool_call>");

  munit_assert_string_equal(captured_permit_tool, "shell");
  munit_assert_string_equal(captured_permit_target, "/repo/project");
  munit_assert_true(strstr(captured_body, "shell llm: pwd") != NULL);
  munit_assert_true(strstr(captured_body, "]<]minimax") == NULL);
  munit_assert_true(strstr(captured_body, "</command>") == NULL);
  munit_assert_true(strstr(captured_body, "</tool_call>") == NULL);
  munit_assert_true(strstr(captured_agent_appends, "]<]minimax") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_minimax_text_tool_call_protocol_error(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/repo/project");
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "minimax/minimax-m3");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_text_done(L, "$ echo ok ]<]minimax[>[</command>]<]minimax[>[</tool_call>");

  munit_assert_string_equal(captured_permit_tool, "");
  munit_assert_true(strstr(captured_body, "shell llm: echo ok") == NULL);
  munit_assert_true(strstr(captured_agent_appends, "]<]minimax") == NULL);
  munit_assert_true(strstr(captured_agent_appends, "</tool_call>") == NULL);
  munit_assert_true(strstr(captured_agent_appends,
                           "provider error: model emitted a textual tool call") != NULL);
  munit_assert_true(strstr(captured_logs, "minimax_text_tool_call_protocol_error") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_minimax_normal_text_is_buffered_until_done(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "minimax/minimax-m3");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(captured_agent_appends, "");

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(captured_agent_appends, "hello");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_arguments_keep_markup_for_non_minimax_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/repo/project");
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_non_minimax_markup", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}]<]minimax[>[</tool_call>");

  munit_assert_string_equal(captured_permit_tool, "");
  munit_assert_true(strstr(captured_agent_appends, "invalid arguments") != NULL);
  munit_assert_true(strstr(captured_body, "Invalid JSON arguments") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_run_permission_skips_later_same_tool_prompts(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_permit_prompt_decision("allow_tool_run");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_tool_run_1", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_shell_tool_run_2", "shell",
                 "{\\\"command\\\":\\\"ls src\\\"}");

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_int(permit_save_calls, ==, 0);
  munit_assert_string_equal(granted_tool, "");
  munit_assert_true(strstr(captured_body, "shell llm: ls src") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_full_run_permission_skips_other_tool_prompts(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_permit_prompt_decision("allow_run");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_full_run_shell", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  send_tool_call(L, "call_full_run_fetch", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_int(permit_save_calls, ==, 0);
  munit_assert_string_equal(granted_tool, "");
  munit_assert_true(strstr(captured_body, "\"tool_call_id\":\"call_full_run_fetch\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_share_child_permission_scope(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_permit_prompt_decision("allow_tool_run");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  tools.handle_tool_calls({}, opts.tools, {{id='inner_' .. opts.messages[1].content, name='shell', arguments='{\\\"command\\\":\\\"pwd\\\"}'}}, '', function() end, {tools = opts.tools, depth = 1, permission_scope = opts.permission_scope, silent_tools = true})\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local current_msgs = {}\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls(current_msgs, available, {{id='call_subs_scope', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"one\\\",\\\"task\\\":\\\"one\\\"},{\\\"id\\\":\\\"two\\\",\\\"task\\\":\\\"two\\\"}],\\\"max_concurrent\\\":2}'}}, '', function() end, {tools = available, depth = 0, permission_scope = {allowed_tools = {}, full_control = false}})\n");
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_int(permit_save_calls, ==, 0);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_shell_tool_display_shows_redacted_command(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *prev_home = getenv("HOME");
  char old_home[4096];
  if (prev_home)
    snprintf(old_home, sizeof(old_home), "%s", prev_home);
  munit_assert_int(setenv("HOME", "/Users/tester", 1), ==, 0);

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/Users/tester/narnia/tui-agent");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_shell_display", "shell",
                 "{\\\"command\\\":\\\"make test\\\"}");

  munit_assert_string_equal(captured_permit_tool, "shell");
  munit_assert_string_equal(captured_permit_target,
                            "/Users/tester/narnia/tui-agent");
  munit_assert_true(strstr(captured_agent_appends, "⚙ shell") != NULL);
  munit_assert_true(strstr(captured_agent_appends,
                           "  $ make test") != NULL);
  munit_assert_true(strstr(captured_logs, "display=shell") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "args={\"command\":\"make test\"}") != NULL);
  munit_assert_true(strstr(captured_body,
                           "{\\\"command\\\":\\\"make test\\\"}") != NULL);

  reset_captures(L);
  lua_close(L);

  if (prev_home)
    munit_assert_int(setenv("HOME", old_home, 1), ==, 0);
  else
    munit_assert_int(unsetenv("HOME"), ==, 0);

  return MUNIT_OK;
}

static MunitResult test_shell_tool_logs_full_redacted_command(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(
      L, "call_shell_long_log", "shell",
      "{\\\"command\\\":\\\"printf alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega tail-marker\\\"}");

  munit_assert_true(strstr(captured_logs, "command=printf alpha beta gamma") !=
                    NULL);
  munit_assert_true(strstr(captured_logs, "tail-marker") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_after_agent_turn_hook_runs_on_final_text(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "after_agent_turn = function(ctx) "
                  "capstan.log('test', 'after_agent_turn text=' .. ctx.text .. ' provider=' .. ctx.provider_name) "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_text_done(L, "done");

  munit_assert_true(strstr(captured_logs, "[test] after_agent_turn text=done provider=deepseek") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_after_agent_turn_hook_waits_for_tool_continuation(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_config_hook(L,
                  "after_agent_turn = function(ctx) "
                  "capstan.log('test', 'after_agent_turn text=' .. ctx.text) "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_wait_shell", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_true(strstr(captured_logs, "[tools] continuing with tool results") != NULL);
  munit_assert_true(strstr(captured_logs, "[test] after_agent_turn") == NULL);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  send_text_done(L, "final");
  munit_assert_true(strstr(captured_logs, "[test] after_agent_turn text=final") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_after_agent_turn_hook_skips_subagent_by_default(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "after_agent_turn = function(ctx) "
                  "capstan.log('test', 'after_agent_turn depth=' .. tostring(ctx.run and ctx.run.depth or 0)) "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");
  lua_newtable(L);
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "depth");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "subtask");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "messages");
  lua_newtable(L);
  rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 4);

  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_text_done(L, "subagent final");
  munit_assert_true(strstr(captured_logs, "[test] after_agent_turn") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_after_agent_turn_hook_scope_all_runs_for_subagent(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "after_agent_turn = { scope = 'all', handler = function(ctx) "
                  "capstan.log('test', 'after_agent_turn depth=' .. tostring(ctx.run and ctx.run.depth or 0)) "
                  "return ctx "
                  "end }");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "agent");
  lua_getfield(L, -1, "run");
  lua_newtable(L);
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "depth");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  lua_pushstring(L, "subtask");
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  lua_setfield(L, -2, "messages");
  lua_newtable(L);
  rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 4);

  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_text_done(L, "subagent final");
  munit_assert_true(strstr(captured_logs, "depth=1 kind=subagent") != NULL);
  munit_assert_true(strstr(captured_logs, "[test] after_agent_turn depth=1") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_config_before_request_hook_mutates_body(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "before_request = function(ctx) "
                  "ctx.request.model = 'hook/model' "
                  "ctx.request.metadata = {tag = 'hooked'} "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"hook/model\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"metadata\":{\"tag\":\"hooked\"}") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_plugin_before_tools_hook_filters_tool_list(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  int rc = luaL_dostring(L,
                         "plugins.fetch.hooks = {"
                         "before_tools = function(ctx) "
                         "local filtered = {} "
                         "for _, tool in ipairs(ctx.tools) do "
                         "if tool['function'].name ~= 'shell' then "
                         "table.insert(filtered, tool) "
                         "end "
                         "end "
                         "ctx.tools = filtered "
                         "return ctx "
                         "end"
                         "}");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"name\":\"fetch\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"file_read\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"name\":\"shell\"") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_call_hooks_mutate_target_and_result(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_config_hook(L,
                  "before_tool_call = function(ctx) "
                  "ctx.target = 'hook-target' "
                  "ctx.args.command = 'echo hook' "
                  "return ctx "
                  "end, "
                  "after_tool_call = function(ctx) "
                  "ctx.result = 'hooked tool result' "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  send_tool_call(L, "call_hook_shell", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_string_equal(captured_permit_target, "hook-target");
  munit_assert_true(strstr(captured_body, "hooked tool result") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_stream_chunk_hook_mutates_text(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "on_stream_chunk = function(ctx) "
                  "if ctx.chunk.type == 'text' then "
                  "ctx.chunk.content = 'hooked text' "
                  "end "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L,
                 "data: {\"choices\":[{\"delta\":{\"content\":\"original\"}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_agent_appends, "hooked text");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_parse_sse_event_override(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  int rc = luaL_dostring(
      L,
      "capstan.config = {providers = {deepseek = {"
      "parse_sse_event = function(raw) "
      "return {type = 'text', content = 'parsed:' .. raw} "
      "end"
      "}}}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, "provider-event\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_agent_appends, "parsed:provider-event");

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_hook_error_logs_and_keeps_request(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "before_request = function(ctx) "
                  "error('boom') "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"deepseek-chat\"") != NULL);
  munit_assert_true(strstr(captured_logs, "[hook] error stage=before_request") != NULL);
  munit_assert_true(strstr(captured_logs, "boom") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/request_enables_auto_tool_choice", test_request_enables_auto_tool_choice,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/request_applies_reasoning_effort", test_request_applies_reasoning_effort,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/config_applies_reasoning_effort", test_config_applies_reasoning_effort,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/agent_reasoning_effort_accessor", test_agent_reasoning_effort_accessor,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/default_profile_is_implement", test_default_profile_is_implement, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/mcp_initializes_lazily_after_startup",
     test_mcp_initializes_lazily_after_startup, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/http_mcp_initializes_without_stdio_spawn",
     test_http_mcp_initializes_without_stdio_spawn, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/disable_mcp_prevents_background_tick",
     test_disable_mcp_prevents_background_tick, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/plan_profile_filters_tools_and_prompt",
     test_plan_profile_filters_tools_and_prompt, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/plan_profile_rejects_unavailable_tool_call",
     test_plan_profile_rejects_unavailable_tool_call, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/fast_profile_applies_reasoning_and_prompt",
     test_fast_profile_applies_reasoning_and_prompt, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_tool_enabled_by_default", test_subagents_tool_enabled_by_default,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_tool_disabled_by_capability",
     test_subagents_tool_disabled_by_capability, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_tool_returns_structured_results",
     test_subagents_tool_returns_structured_results, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/plan_subagents_inherit_profile_and_readonly_tools",
     test_plan_subagents_inherit_profile_and_readonly_tools, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_respect_explicit_small_max_turns",
     test_subagents_respect_explicit_small_max_turns, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_pass_shared_instructions_to_children",
     test_subagents_pass_shared_instructions_to_children, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_wait_loop_yields_between_polls",
     test_subagents_wait_loop_yields_between_polls, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_retry_transient_http_errors",
     test_subagents_retry_transient_http_errors, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_do_not_retry_http_400",
     test_subagents_do_not_retry_http_400, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_use_current_provider_models_only",
     test_subagents_use_current_provider_models_only, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_config_sets_default_model",
     test_provider_config_sets_default_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_state_model_overrides_config_model",
     test_provider_state_model_overrides_config_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/config_profile_model_selected_for_profile",
     test_config_profile_model_selected_for_profile, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/runtime_startup_publishes_configured_profile_status",
     test_runtime_startup_publishes_configured_profile_status, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/state_profile_model_overrides_config_profile_model",
     test_state_profile_model_overrides_config_profile_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/explicit_model_overrides_profile_model",
     test_explicit_model_overrides_profile_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_state_provider_overrides_config_provider",
     test_provider_state_provider_overrides_config_provider, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_env_provider_overrides_state_provider",
     test_provider_env_provider_overrides_state_provider, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_env_model_overrides_state_model",
     test_provider_env_model_overrides_state_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_set_for_persists_active_provider",
     test_provider_models_set_for_persists_active_provider, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_set_persists_state_file",
     test_provider_models_set_persists_state_file, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_config_sets_weak_model",
     test_provider_config_sets_weak_model, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/provider_state_weak_model_overrides_config",
     test_provider_state_weak_model_overrides_config, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_set_weak_persists_state_file",
     test_provider_models_set_weak_persists_state_file, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_set_profile_persists_state_file",
     test_provider_models_set_profile_persists_state_file, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/compact_uses_weak_model_and_replaces_history",
     test_compact_uses_weak_model_and_replaces_history, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_list_uses_api_response",
     test_provider_models_list_uses_api_response, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_list_all_includes_static_config_models",
     test_provider_models_list_all_includes_static_config_models, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_endpoint_derived_from_chat_endpoint",
     test_provider_models_endpoint_derived_from_chat_endpoint, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/fetch_permission_target_uses_url", test_fetch_permission_target_uses_url,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/logs_text_when_model_does_not_call_tool",
     test_logs_text_when_model_does_not_call_tool, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_error_finishes_with_error",
     test_stream_error_finishes_with_error, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/file_read_permission_target_uses_path",
     test_file_read_permission_target_uses_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/sensitive_file_read_forces_prompt",
     test_sensitive_file_read_forces_prompt, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/chunked_tool_call_arguments_continue_with_tool_result",
     test_chunked_tool_call_arguments_continue_with_tool_result, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_tool_delta_logs_only_at_debug",
     test_stream_tool_delta_logs_only_at_debug, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/streamed_file_edit_tool_edits_file",
     test_streamed_file_edit_tool_edits_file, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_tool_uses_tool_args_path",
     test_file_read_tool_uses_tool_args_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_tool_allows_permitted_outside_path",
     test_file_read_tool_allows_permitted_outside_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_write_permission_target_uses_absolute_workspace_path",
     test_file_write_permission_target_uses_absolute_workspace_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/invalid_tool_arguments_skip_permission",
     test_invalid_tool_arguments_skip_permission, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_handler_error_returns_tool_result",
     test_tool_handler_error_returns_tool_result, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_stops_repeated_shell_command",
     test_tool_guard_stops_repeated_shell_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_allows_repeated_shell_by_default",
     test_tool_guard_allows_repeated_shell_by_default, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_shell_repeat_resets_after_other_tool",
     test_tool_guard_shell_repeat_resets_after_other_tool, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_generic_repeat_resets_after_shell_tool",
     test_tool_guard_generic_repeat_resets_after_shell_tool, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_stops_repeated_tool_call",
     test_tool_guard_stops_repeated_tool_call, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_stops_total_tool_call_budget",
     test_tool_guard_stops_total_tool_call_budget, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_stops_duration_before_tool_execution",
     test_tool_guard_stops_duration_before_tool_execution, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_stops_max_turns_before_continuation_request",
     test_tool_guard_stops_max_turns_before_continuation_request, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_always_allow_uses_workspace_target",
     test_shell_always_allow_uses_workspace_target, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_tool_redacts_command_before_continuation",
     test_shell_tool_redacts_command_before_continuation, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_arguments_strip_minimax_markup",
     test_tool_arguments_strip_minimax_markup, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/minimax_text_tool_call_protocol_error",
     test_minimax_text_tool_call_protocol_error, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/minimax_normal_text_is_buffered_until_done",
     test_minimax_normal_text_is_buffered_until_done, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_arguments_keep_markup_for_non_minimax_model",
     test_tool_arguments_keep_markup_for_non_minimax_model, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_run_permission_skips_later_same_tool_prompts",
     test_tool_run_permission_skips_later_same_tool_prompts, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/full_run_permission_skips_other_tool_prompts",
     test_full_run_permission_skips_other_tool_prompts, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_share_child_permission_scope",
     test_subagents_share_child_permission_scope, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_tool_display_shows_redacted_command",
     test_shell_tool_display_shows_redacted_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_tool_logs_full_redacted_command",
     test_shell_tool_logs_full_redacted_command, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/after_agent_turn_hook_runs_on_final_text",
     test_after_agent_turn_hook_runs_on_final_text, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/after_agent_turn_hook_waits_for_tool_continuation",
     test_after_agent_turn_hook_waits_for_tool_continuation, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/after_agent_turn_hook_skips_subagent_by_default",
     test_after_agent_turn_hook_skips_subagent_by_default, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/after_agent_turn_hook_scope_all_runs_for_subagent",
     test_after_agent_turn_hook_scope_all_runs_for_subagent, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/config_before_request_hook_mutates_body",
     test_config_before_request_hook_mutates_body, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/plugin_before_tools_hook_filters_tool_list",
     test_plugin_before_tools_hook_filters_tool_list, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_call_hooks_mutate_target_and_result",
     test_tool_call_hooks_mutate_target_and_result, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_chunk_hook_mutates_text",
     test_stream_chunk_hook_mutates_text, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_parse_sse_event_override",
     test_provider_parse_sse_event_override, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/hook_error_logs_and_keeps_request",
     test_hook_error_logs_and_keeps_request, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite provider_tools_suite = {"/provider_tools", tests, NULL, 1,
                                   MUNIT_SUITE_OPTION_NONE};
