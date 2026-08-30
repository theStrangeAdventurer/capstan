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
#define CAPTURED_BODY_SIZE (2 * 1024 * 1024)
static char captured_body[CAPTURED_BODY_SIZE];
static char captured_get_url[512];
static char captured_permit_tool[128];
static char captured_permit_target[512];
static char captured_logs[8192];
static char captured_agent_appends[2048];
static char last_agent_provider[128];
static char last_agent_model[128];
static char last_agent_reasoning_effort[32];
static char last_agent_profile[128];
static char captured_activity_during_tool[128];
static char temp_state_path[512];
static const char *permit_decision = "deny";
static const char *permit_prompt_decision = "always";
static char granted_tool[128];
static char granted_pattern[512];
static int grant_allow;
static int permit_prompt_calls;
static int permit_check_calls;
static int permit_save_calls;
static int permit_explicit_allow;
static int permit_prompt_advance_ms;
static int mock_models_success;
static int post_stream_calls;
static lua_Integer captured_stream_timeout_ms;
static int captured_stream_background;

static int l_http_get(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  strncpy(captured_get_url, url, sizeof(captured_get_url) - 1);
  captured_get_url[sizeof(captured_get_url) - 1] = '\0';
  if (mock_models_success) {
    lua_pushinteger(L, 200);
    lua_pushstring(L,
                   "{\"data\":["
                   "{\"id\":\"~model/z-latest\",\"name\":\"Model Z Latest\","
                   "\"context_length\":12345},"
                   "{\"id\":\"model/a\",\"name\":\"Model A\","
                   "\"supported_parameters\":[\"reasoning\"]}"
                   "]}");
    return 2;
  }
  lua_pushinteger(L, 500);
  lua_pushstring(L, "");
  return 2;
}

static int l_http_post_stream(lua_State *L) {
  post_stream_calls++;
  captured_stream_timeout_ms = luaL_optinteger(L, 5, 0);
  captured_stream_background = 0;
  if (lua_istable(L, 6)) {
    lua_getfield(L, 6, "background");
    captured_stream_background = lua_toboolean(L, -1);
    lua_pop(L, 1);
  }
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
    lua_pushboolean(L, 1);
    return 2;
  }
  lua_pushstring(L, permit_decision);
  lua_pushboolean(L, permit_explicit_allow);
  return 2;
}

static int l_permit_prompt(lua_State *L) {
  permit_prompt_calls++;
  if (permit_prompt_advance_ms > 0) {
    lua_getglobal(L, "MOCK_NOW_MS");
    lua_Integer now = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_pushinteger(L, now + permit_prompt_advance_ms);
    lua_setglobal(L, "MOCK_NOW_MS");
  }
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
  const char *reasoning_effort = luaL_optstring(L, 3, "");
  strncpy(last_agent_provider, provider, sizeof(last_agent_provider) - 1);
  last_agent_provider[sizeof(last_agent_provider) - 1] = '\0';
  strncpy(last_agent_model, model, sizeof(last_agent_model) - 1);
  last_agent_model[sizeof(last_agent_model) - 1] = '\0';
  strncpy(last_agent_reasoning_effort, reasoning_effort,
          sizeof(last_agent_reasoning_effort) - 1);
  last_agent_reasoning_effort[sizeof(last_agent_reasoning_effort) - 1] = '\0';
  return 0;
}

static int l_agent_set_profile_info(lua_State *L) {
  const char *profile = luaL_optstring(L, 1, "");
  strncpy(last_agent_profile, profile, sizeof(last_agent_profile) - 1);
  last_agent_profile[sizeof(last_agent_profile) - 1] = '\0';
  return 0;
}

static int l_capture_activity(lua_State *L) {
  (void)L;
  snprintf(captured_activity_during_tool,
           sizeof(captured_activity_during_tool), "%s", agent_activity());
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
  lua_pushcfunction(L, l_agent_append);
  lua_setfield(L, -2, "append_ui");
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

  lua_pushcfunction(L, l_capture_activity);
  lua_setglobal(L, "capture_activity");

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
                "handler = function(ctx) capture_activity(); return ctx:replace('ui', 'llm') end"
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
                "handler = function(ctx) "
                "if ctx.tool_args.command == 'npm test --fail' then return 'shell ui', '[exit 1]', false end "
                "return ctx:replace('shell ui', 'shell llm: ' .. ctx.tool_args.command) end"
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
  last_agent_reasoning_effort[0] = '\0';
  last_agent_profile[0] = '\0';
  captured_activity_during_tool[0] = '\0';
  agent_set_activity(NULL);
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
  permit_explicit_allow = 0;
  permit_prompt_advance_ms = 0;
  mock_models_success = 0;
  post_stream_calls = 0;
  captured_stream_timeout_ms = 0;
  captured_stream_background = 0;
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

static void set_capstan_workspace_root(lua_State *L, const char *path) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  lua_pushstring(L, path);
  lua_setfield(L, -2, "workspace_root");
  lua_setglobal(L, "capstan");
}

static void set_capstan_wiki_path(lua_State *L, const char *path) {
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
  lua_pushstring(L, path);
  lua_setfield(L, -2, "path");
  lua_setfield(L, -2, "wiki");
  lua_setfield(L, -2, "config");
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

static void write_file_bytes(const char *path, const unsigned char *content,
                             size_t size) {
  FILE *f = fopen(path, "wb");
  munit_assert_not_null(f);
  munit_assert_size(fwrite(content, 1, size, f), ==, size);
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
static void send_reasoning_delta(lua_State *L, const char *reasoning,
                                 int include_details);
static void send_reasoning_detail_fragment(lua_State *L, const char *text,
                                           const char *signature);
static void send_text_delta(lua_State *L, const char *text);
static void send_text_done(lua_State *L, const char *text);

static MunitResult test_model_start_callback_reports_effective_request_context(
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
      "local ok, err = capstan.agent.run({"
      "messages = {{role = 'user', content = 'inspect'}}, "
      "reasoning_effort = 'high', preserve_reasoning = false}, {"
      "on_model_start = function(turn, attempt, messages, tools, prompt, "
      "provider, model, effort, profile, preserve, body_bytes, purpose) "
      "model_start_snapshot = {turn = turn, attempt = attempt, "
      "provider = provider, model = model, effort = effort, profile = profile, "
      "preserve = preserve, body_bytes = body_bytes, purpose = purpose} end}) "
      "assert(ok, err)");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "model_start_snapshot");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "turn");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getfield(L, -1, "attempt");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getfield(L, -1, "provider");
  munit_assert_string_equal(lua_tostring(L, -1), "deepseek");
  lua_pop(L, 1);
  lua_getfield(L, -1, "model");
  munit_assert_true(lua_isstring(L, -1));
  munit_assert_size(lua_rawlen(L, -1), >, 0);
  lua_pop(L, 1);
  lua_getfield(L, -1, "effort");
  munit_assert_string_equal(lua_tostring(L, -1), "high");
  lua_pop(L, 1);
  lua_getfield(L, -1, "profile");
  munit_assert_string_equal(lua_tostring(L, -1), "implement");
  lua_pop(L, 1);
  lua_getfield(L, -1, "preserve");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "body_bytes");
  munit_assert_int((int)lua_tointeger(L, -1), >, 0);
  lua_pop(L, 1);
  lua_getfield(L, -1, "purpose");
  munit_assert_string_equal(lua_tostring(L, -1), "agent");
  lua_pop(L, 2);
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_model_observer_failure_stops_run_cleanly(
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
      "local ok, err = capstan.agent.run({"
      "messages = {{role = 'user', content = 'inspect'}}}, {"
      "on_model_start = function() error('observer boom') end, "
      "on_done = function(result) observer_failure_result = result end}) "
      "assert(ok, err)");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "observer_failure_result");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "ok");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "error");
  munit_assert_not_null(strstr(lua_tostring(L, -1),
                               "on_model_start failed"));
  lua_pop(L, 2);
  munit_assert_int(stream_callback_ref, ==, LUA_NOREF);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_sync_post_stream_failure_finishes_cleanly(
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
      "http.post_stream = function() error('sync setup boom') end "
      "local starts, finishes = 0, 0 "
      "local ok, err = capstan.agent.run({"
      "messages = {{role = 'user', content = 'inspect'}}}, {"
      "on_model_start = function() starts = starts + 1 end, "
      "on_model_done = function() finishes = finishes + 1 end, "
      "on_done = function(result) sync_failure_result = result; "
      "sync_failure_starts = starts; sync_failure_finishes = finishes end}) "
      "assert(ok, err)");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "sync_failure_result");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "ok");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "error");
  munit_assert_not_null(strstr(lua_tostring(L, -1),
                               "HTTP stream setup failed"));
  lua_pop(L, 2);
  lua_getglobal(L, "sync_failure_starts");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "sync_failure_finishes");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_interactive_options_override_request(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dostring(
      L,
      "local ok, err = capstan.agent.configure_interactive({"
      "provider = 'openrouter', model = 'cli/model', "
      "reasoning_effort = 'high', max_turns = 7, "
      "preserve_reasoning = false}) "
      "assert(ok, err)");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"cli/model\"") !=
                    NULL);
  munit_assert_true(strstr(captured_body, "\"effort\":\"high\"") !=
                    NULL);
  munit_assert_string_equal(last_agent_provider, "openrouter");
  munit_assert_string_equal(last_agent_model, "cli/model");
  munit_assert_string_equal(last_agent_reasoning_effort, "high");

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_interactive_options_reject_unknown_provider(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dostring(
      L,
      "local ok, err = capstan.agent.configure_interactive({provider = "
      "'missing'}) "
      "assert(ok == nil and err:find('unknown provider', 1, true))");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(post_stream_calls, ==, 0);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_interactive_options_reject_unknown_profile(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dostring(
      L,
      "local ok, err = capstan.agent.configure_interactive({profile = "
      "'missing'}) "
      "assert(ok == nil and err:find('unknown profile', 1, true))");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(post_stream_calls, ==, 0);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_interactive_cli_profile_can_be_switched(
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
      "assert(capstan.agent.configure_interactive({profile = 'implement'})) "
      "assert(capstan.agent.set_profile('plan') == 'plan')");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);
  munit_assert_string_equal(last_agent_profile, "plan");
  munit_assert_null(strstr(captured_body, "\"name\":\"shell\""));

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_profiles_isolation_and_default_replacement(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  int rc = luaL_dostring(
      L,
      "package.loaded['agent.profiles'] = nil\n"
      "capstan.runtime_options = {isolated = true}\n"
      "capstan.config = {agent = {profiles = {evil = {default = true}}}}\n"
      "local isolated = require('agent.profiles')\n"
      "assert(isolated.get('evil') == nil)\n"
      "assert(isolated.default_name() == 'implement')\n"
      "package.loaded['agent.profiles'] = nil\n"
      "capstan.runtime_options = {}\n"
      "capstan.config = {agent = {profiles = {implement = {default = false}, "
      "fast = {default = true}}}}\n"
      "local configured = require('agent.profiles')\n"
      "assert(configured.default_name() == 'fast')\n"
      "package.loaded['agent.profiles'] = nil\n"
      "capstan.config = {agent = {profiles = {\n"
      "  bad_tools = {allowed_tools = false},\n"
      "  bad_prompt = {prompt = true},\n"
      "  nested_prompt = {prompt = {{'nested'}}},\n"
      "  bad_append = {prompt_append = {}}\n"
      "}}}\n"
      "local validated = require('agent.profiles')\n"
      "assert(validated.get('bad_tools') == nil)\n"
      "assert(validated.get('bad_prompt') == nil)\n"
      "assert(validated.get('nested_prompt') == nil)\n"
      "assert(validated.get('bad_append') == nil)\n");
  munit_assert_int(rc, ==, LUA_OK);
  lua_close(L);
  return MUNIT_OK;
}

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

  munit_assert_true(strstr(captured_body, "\"reasoning_effort\":\"low\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning\":{\"effort\"") == NULL);
  munit_assert_true(strstr(captured_body, "Reasoning effort: low") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_openrouter_keeps_nested_reasoning_effort(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  call_agent_run_with_reasoning_effort(L, "low");

  munit_assert_true(strstr(captured_body,
                           "\"reasoning\":{\"effort\":\"low\"}") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning_effort\"") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_reasoning_details_preserved_for_tool_continuation(
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
  send_reasoning_delta(L, "inspect files", 1);
  send_tool_call(L, "call_reasoning_details", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_true(strstr(captured_body, "\"reasoning_details\":[{") != NULL);
  munit_assert_true(strstr(captured_body, "\"text\":\"inspect files\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning\":\"inspect files\"") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_stream_merges_reasoning_detail_fragments(
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
  send_reasoning_detail_fragment(L, "inspect ", NULL);
  send_reasoning_detail_fragment(L, "files", "signed-block");
  send_tool_call(L, "call_fragmented_reasoning", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  const char *detail = strstr(captured_body, "\"type\":\"reasoning.text\"");
  munit_assert_not_null(detail);
  munit_assert_null(strstr(detail + 1, "\"type\":\"reasoning.text\""));
  munit_assert_not_null(strstr(captured_body, "\"text\":\"inspect files\""));
  munit_assert_not_null(strstr(captured_body,
                               "\"signature\":\"signed-block\""));

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_reasoning_plaintext_preserved_and_config_can_disable(
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
  send_reasoning_delta(L, "continue thought", 0);
  send_tool_call(L, "call_reasoning_text", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  munit_assert_true(strstr(captured_body,
                           "\"reasoning\":\"continue thought\"") != NULL);
  reset_captures(L);
  lua_close(L);

  L = new_provider_state();
  reset_captures(L);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  call_agent_entry(L);
  send_reasoning_delta(L, "deepseek thought", 0);
  send_tool_call(L, "call_deepseek_reasoning", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  munit_assert_true(strstr(captured_body,
                           "\"reasoning_content\":\"deepseek thought\"") != NULL);
  munit_assert_true(strstr(captured_body,
                           "\"reasoning\":\"deepseek thought\"") == NULL);
  reset_captures(L);
  lua_close(L);

  L = new_provider_state();
  reset_captures(L);
  rc = luaL_dostring(
      L,
      "capstan.config = capstan.config or {} "
      "capstan.config.agent = {preserve_reasoning = false}");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  call_agent_entry(L);
  send_reasoning_delta(L, "disabled thought", 1);
  send_tool_call(L, "call_reasoning_disabled", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");
  munit_assert_true(strstr(captured_body, "disabled thought") == NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning_details\":[{") == NULL);

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

  munit_assert_true(strstr(captured_body, "\"reasoning_effort\":\"minimal\"") != NULL);
  munit_assert_string_equal(last_agent_reasoning_effort, "minimal");
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

  char system_prompt[8192];
  read_file("ai/system_prompt.txt", system_prompt, sizeof(system_prompt));
  lua_pushstring(L, system_prompt);
  lua_setglobal(L, "system_prompt");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  munit_assert_string_equal(last_agent_profile, "implement");

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "Active Profile: Implement") != NULL);
  munit_assert_true(strstr(captured_body, "Form one concrete hypothesis privately") != NULL);
  munit_assert_true(strstr(captured_body, "without a separate planning response") != NULL);
  munit_assert_true(strstr(captured_body, "batch-capable tools") != NULL);
  munit_assert_true(strstr(captured_body, "preserve them and ask before") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning_effort\":\"medium\"") != NULL);
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

static MunitResult test_mcp_image_result_becomes_multimodal_message(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_mcp_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  lua_getglobal(L, "capstan");
  lua_getfield(L, -1, "mcp");
  lua_getfield(L, -1, "tick");
  lua_pushinteger(L, 2);
  rc = lua_pcall(L, 1, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 3);

  reset_captures(L);
  call_agent_entry(L);
  set_permit_decision("allow");
  rc = luaL_dostring(
      L,
      "mcp.recv = function(handle, timeout)\n"
      "  return '{\"jsonrpc\":\"2.0\",\"id\":' .. MCP_LAST_ID .. "
      "',\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Screenshot ready\"},' .. "
      "'{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII=\"}]}}'\n"
      "end\n"
      "mcp.recv_nowait = function(handle) return mcp.recv(handle, 0) end\n");
  munit_assert_int(rc, ==, LUA_OK);

  send_tool_call(L, "call_image", "mcp__stub__demo", "{}");

  munit_assert_true(strstr(captured_body, "\"role\":\"tool\"") != NULL);
  munit_assert_true(strstr(captured_body, "Screenshot ready") != NULL);
  munit_assert_true(strstr(captured_body, "\"type\":\"image_url\"") != NULL);
  munit_assert_true(strstr(
      captured_body,
      "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII=") != NULL);
  munit_assert_true(strstr(captured_logs, "iVBORw0KGgo") == NULL);

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

static MunitResult test_http_mcp_initialize_error_marks_server_failed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_http_mcp_config(L);

  int rc = luaL_dostring(
      L,
      "http.post_response_async = function(url, body, headers, timeout, callback, opts)\n"
      "  callback({status = 401, body = 'Unauthorized', headers = {['content-type'] = 'text/plain'}})\n"
      "  return 1\n"
      "end\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  call_agent_entry(L);

  rc = luaL_dostring(
      L,
      "capstan.mcp.tick(2)\n"
      "local server = require('agent.mcp').list_servers()[1]\n"
      "MCP_FAILED_STATUS = server.status\n"
      "MCP_FAILED_ERROR = server.error\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "MCP_FAILED_STATUS");
  munit_assert_string_equal(lua_tostring(L, -1), "failed");
  lua_pop(L, 1);
  lua_getglobal(L, "MCP_FAILED_ERROR");
  munit_assert_true(strstr(lua_tostring(L, -1),
                           "initialize failed: HTTP 401: Unauthorized") != NULL);
  lua_pop(L, 1);

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
  munit_assert_true(strstr(captured_body, "\"reasoning_effort\":\"high\"") != NULL);
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

static MunitResult test_builtin_profiles_are_plan_and_implement(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();

  int rc = luaL_dostring(
      L,
      "capstan.runtime_options = {isolated = true}\n"
      "local profiles = require('agent.profiles')\n"
      "local names = profiles.names()\n"
      "assert(#names == 2)\n"
      "assert(names[1] == 'implement')\n"
      "assert(names[2] == 'plan')\n"
      "assert(profiles.get('implement').completion_review == false)\n"
      "assert(profiles.get('fast') == nil)\n");
  munit_assert_int(rc, ==, LUA_OK);

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

static MunitResult test_subagents_reject_unavailable_tools_before_launch(
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
      "subagents_child_runs = 0\n"
      "subagents_invalid_result = ''\n"
      "capstan.agent.run = function() subagents_child_runs = subagents_child_runs + 1; return false, 'must not run' end\n"
      "local current_msgs = {}\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls(current_msgs, available, {{id='call_invalid_subs', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"valid\\\",\\\"task\\\":\\\"read docs\\\",\\\"tools\\\":[\\\"fetch\\\"]},{\\\"id\\\":\\\"bad\\\",\\\"task\\\":\\\"inspect code\\\",\\\"tools\\\":[\\\"functions.shell\\\",\\\"functions.file_read\\\"]}]}' }}, '', function(msgs)\n"
      "  subagents_invalid_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagents_child_runs");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 0);
  lua_pop(L, 1);

  lua_getglobal(L, "subagents_invalid_result");
  const char *result = lua_tostring(L, -1);
  munit_assert_not_null(result);
  munit_assert_true(strstr(result, "task \"bad\" requests unavailable tools: functions.file_read, functions.shell") != NULL);
  munit_assert_true(strstr(result, "Available tools: fetch, file_read, shell") != NULL);
  lua_pop(L, 1);
  munit_assert_true(strstr(captured_agent_appends, "subagents: running") == NULL);

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
      "tools.handle_tool_calls({}, available, {{id='call_plan_subs', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"a\\\",\\\"task\\\":\\\"inspect a\\\"},{\\\"id\\\":\\\"b\\\",\\\"task\\\":\\\"inspect b\\\",\\\"tools\\\":[\\\"fetch\\\"]}],\\\"max_concurrent\\\":2}'}}, '', function() end, {tools = available, depth = 0, profile = 'plan'})\n");
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

static MunitResult test_subagents_cap_model_requested_limits(
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
      "capstan.config = capstan.config or {}\n"
      "capstan.config.subagents = {max_turns_cap = 3, max_concurrent_cap = 2}\n"
      "subagent_capped_turns = {}\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  table.insert(subagent_capped_turns, opts.max_turns)\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_capped_limits', name='subagents', arguments='{\\\"max_concurrent\\\":50,\\\"tasks\\\":[{\\\"id\\\":\\\"one\\\",\\\"task\\\":\\\"one\\\",\\\"max_turns\\\":50},{\\\"id\\\":\\\"two\\\",\\\"task\\\":\\\"two\\\",\\\"max_turns\\\":50}]}' }}, '', function() end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_capped_turns");
  lua_rawgeti(L, -1, 1);
  munit_assert_int((int)lua_tointeger(L, -1), ==, 3);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  munit_assert_int((int)lua_tointeger(L, -1), ==, 3);
  lua_pop(L, 2);
  munit_assert_true(strstr(captured_agent_appends, "running 2 concurrent") != NULL);

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

static MunitResult test_subagents_discard_failed_output_and_raw_error_body(
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
      "capstan.config = {subagents = {max_attempts = 1}}\n"
      "subagent_result = ''\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  callbacks.on_done({ok = false, error = 'Connection error: Timeout was reached\\nevent: response.created\\ndata: SECRET_SYSTEM_PROMPT', text = 'SECRET_SYSTEM_PROMPT', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_raw_error', name='subagents', arguments='{\"tasks\":[{\"id\":\"failed\",\"task\":\"failed\"}]}' }}, '', function(msgs)\n"
      "  subagent_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_result");
  const char *result = lua_tostring(L, -1);
  munit_assert_not_null(result);
  munit_assert_true(strstr(result, "\"error\":\"Connection error: Timeout was reached\"") != NULL);
  munit_assert_true(strstr(result, "\"text\":\"\"") != NULL);
  munit_assert_true(strstr(result, "SECRET_SYSTEM_PROMPT") == NULL);
  munit_assert_true(strstr(result, "response.created") == NULL);
  lua_pop(L, 1);

  munit_assert_true(strstr(captured_agent_appends, "SECRET_SYSTEM_PROMPT") == NULL);
  munit_assert_true(strstr(captured_agent_appends, "response.created") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_subagents_bound_successful_output_with_valid_utf8(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dostring(
      L,
      "local json = require('vendor.rxi.json')\n"
      "local tools = require('agent.tools')\n"
      "capstan.config = {subagents = {max_result_bytes = 256}}\n"
      "subagent_result = ''\n"
      "subagent_text = ''\n"
      "subagent_original_bytes = 0\n"
      "subagent_was_truncated = false\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  callbacks.on_done({ok = true, text = string.rep('я', 200), turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_subs_bounded', name='subagents', arguments='{\"tasks\":[{\"id\":\"bounded\",\"task\":\"bounded\"}]}' }}, '', function(msgs)\n"
      "  subagent_result = msgs[#msgs].content\n"
      "end, {tools = available, depth = 0})\n"
      "local decoded = json.decode(subagent_result)\n"
      "subagent_text = decoded.results[1].text\n"
      "subagent_original_bytes = decoded.results[1].text_original_bytes or 0\n"
      "subagent_was_truncated = decoded.results[1].text_truncated == true\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "subagent_text");
  size_t text_len = 0;
  (void)lua_tolstring(L, -1, &text_len);
  munit_assert_size(text_len, <=, 256);
  lua_pop(L, 1);
  lua_getglobal(L, "subagent_original_bytes");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 400);
  lua_pop(L, 1);
  lua_getglobal(L, "subagent_was_truncated");
  munit_assert_true(lua_toboolean(L, -1));
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
  munit_assert_string_equal(last_agent_reasoning_effort, "high");
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
  setenv("CAPSTAN_PROVIDER", "openrouter", 1);
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_state_provider(L, "deepseek");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"config/model\"") != NULL);

  unsetenv("CAPSTAN_PROVIDER");
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_env_model_overrides_state_model(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  setenv("CAPSTAN_PROVIDER", "openrouter", 1);
  setenv("CAPSTAN_MODEL", "env/model", 1);
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);
  set_capstan_state_model(L, "openrouter", "state/model");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "\"model\":\"env/model\"") != NULL);

  unsetenv("CAPSTAN_MODEL");
  unsetenv("CAPSTAN_PROVIDER");
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_env_context_limit_applies_to_active_provider(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  setenv("CAPSTAN_PROVIDER", "openrouter", 1);
  setenv("CAPSTAN_CONTEXT_LIMIT", "4242", 1);
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dostring(
      L,
      "local runtime = require('agent.provider_config').build()\n"
      "env_context_limit = runtime.providers.openrouter.context_limit\n");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "env_context_limit");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 4242);
  lua_pop(L, 1);

  unsetenv("CAPSTAN_CONTEXT_LIMIT");
  unsetenv("CAPSTAN_PROVIDER");
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
  lua_pushstring(L, "deepseek-v4-pro");
  lua_pushstring(L, "high");
  rc = lua_pcall(L, 3, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));
  munit_assert_string_equal(last_agent_reasoning_effort, "high");

  lua_getfield(L, -3, "current_provider");
  rc = lua_pcall(L, 0, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(lua_tostring(L, -1), "deepseek");

  char contents[768];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "provider = \"deepseek\"") != NULL);
  munit_assert_true(strstr(contents,
                           "[\"deepseek\"] = \"deepseek-v4-pro\"") != NULL);
  munit_assert_true(strstr(contents,
                           "[\"deepseek\"] = \"high\"") != NULL);

  call_agent_entry(L);
  munit_assert_true(strstr(captured_body, "\"model\":\"deepseek-v4-pro\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"reasoning_effort\":\"high\"") != NULL);

  unlink(temp_state_path);
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_models_set_publishes_effective_effort(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dostring(
      L, "capstan.config.agent = {reasoning_effort = 'minimal'}");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  rc = luaL_dostring(
      L, "models_set_ok, models_set_err = capstan.models.set('effective/model')");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "models_set_ok");
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  munit_assert_string_equal(last_agent_reasoning_effort, "minimal");

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
  lua_pushstring(L, "default");
  rc = lua_pcall(L, 2, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));
  munit_assert_string_equal(last_agent_reasoning_effort, "default");

  char contents[512];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "[\"openrouter\"] = \"persisted/model\"") != NULL);
  munit_assert_true(strstr(contents,
                           "[\"openrouter\"] = \"default\"") != NULL);

  call_agent_entry(L);
  munit_assert_true(strstr(captured_body, "\"reasoning\":") == NULL);

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
  lua_pushstring(L, "low");
  rc = lua_pcall(L, 3, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));

  char contents[768];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "weak_model = {") != NULL);
  munit_assert_true(strstr(contents, "provider = \"openrouter\"") != NULL);
  munit_assert_true(strstr(contents, "model = \"persisted/weak\"") != NULL);
  munit_assert_true(strstr(contents, "reasoning_effort = \"low\"") != NULL);

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
  lua_pushstring(L, "medium");
  rc = lua_pcall(L, 4, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -2));

  char contents[1024];
  read_file(temp_state_path, contents, sizeof(contents));
  munit_assert_true(strstr(contents, "profile_models = {") != NULL);
  munit_assert_true(strstr(contents, "[\"plan\"] = {") != NULL);
  munit_assert_true(strstr(contents, "provider = \"openrouter\"") != NULL);
  munit_assert_true(strstr(contents, "model = \"persisted/plan\"") != NULL);
  munit_assert_true(strstr(contents,
                           "reasoning_effort = \"medium\"") != NULL);

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
  set_config_hook(L,
                  "after_agent_turn = function(ctx) "
                  "compact_after_turn_calls = "
                  "(compact_after_turn_calls or 0) + 1 "
                  "return ctx "
                  "end");

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
  munit_assert_true(agent_is_running());
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  munit_assert_true(strstr(captured_body, "\"model\":\"weak/model\"") != NULL);
  munit_assert_true(strstr(captured_body, "\"tools\"") == NULL);
  munit_assert_true(strstr(captured_body, "operational handoff summary") != NULL);

  send_text_done(L, "Goal: continue compact work");

  munit_assert_false(agent_is_running());
  lua_getglobal(L, "compact_after_turn_calls");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);

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

static MunitResult test_compact_whitespace_result_preserves_history(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  clear_messages();
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  char *user = malloc(strlen("keep this user message") + 1);
  char *assistant = malloc(strlen("keep this assistant response") + 1);
  munit_assert_not_null(user);
  munit_assert_not_null(assistant);
  strcpy(user, "keep this user message");
  strcpy(assistant, "keep this assistant response");
  add_message(user, user, MSG_USER);
  add_message(assistant, assistant, MSG_AGENT);

  agent_compact(L);
  munit_assert_true(agent_is_running());
  send_text_done(L, "   ");

  munit_assert_false(agent_is_running());
  Messages *msgs = get_messages();
  munit_assert_size(msgs->size, ==, 2);
  munit_assert_string_equal(msgs->items[0]->text, "keep this user message");
  munit_assert_string_equal(msgs->items[1]->text,
                            "keep this assistant response");
  munit_assert_string_equal(captured_agent_appends, "");

  clear_messages();
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_compact_provider_error_preserves_history_and_finishes(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  clear_messages();
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  char *user = malloc(strlen("original compact context") + 1);
  munit_assert_not_null(user);
  strcpy(user, "original compact context");
  add_message(user, user, MSG_USER);

  agent_compact(L);
  munit_assert_true(agent_is_running());
  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  lua_pushstring(L, "HTTP 400");
  lua_pushstring(L, "{\"error\":{\"message\":\"invalid compact request\"}}");
  rc = lua_pcall(L, 4, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_false(agent_is_running());
  Messages *msgs = get_messages();
  munit_assert_size(msgs->size, ==, 1);
  munit_assert_string_equal(msgs->items[0]->text,
                            "original compact context");
  munit_assert_string_equal(captured_agent_appends, "");

  clear_messages();
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_auto_compact_estimates_pending_request_and_can_disable(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  clear_messages();
  set_capstan_provider_config(L);
  int rc = luaL_dostring(
      L,
      "capstan.config.providers.openrouter.context_limit = 100000\n"
      "capstan.config.agent = {auto_compact_percent = 1}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  char *user = malloc(strlen("short existing context") + 1);
  munit_assert_not_null(user);
  strcpy(user, "short existing context");
  add_message(user, user, MSG_USER);

  munit_assert_false(agent_should_auto_compact(L, ""));

  char pending[8192];
  memset(pending, 'x', sizeof(pending) - 1);
  pending[sizeof(pending) - 1] = '\0';
  munit_assert_true(agent_should_auto_compact(L, pending));
  munit_assert_true(strstr(captured_logs,
                           "[compact] auto_check estimated_tokens=") != NULL);
  munit_assert_true(strstr(captured_logs, "threshold=1 trigger=true") != NULL);

  rc = luaL_dostring(L, "capstan.config.agent.auto_compact_percent = 0");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_false(agent_should_auto_compact(L, pending));

  clear_messages();
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_token_estimate_is_conservative_for_utf8(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "local tokens = require('agent.tokens')\n"
      "ascii_token_estimate = tokens.estimate_text_tokens('test')\n"
      "utf8_token_estimate = tokens.estimate_text_tokens('тест')\n"
      "cjk_token_estimate = tokens.estimate_text_tokens('上下文')\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "ascii_token_estimate");
  munit_assert_int(lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "utf8_token_estimate");
  munit_assert_int(lua_tointeger(L, -1), ==, 2);
  lua_pop(L, 1);
  lua_getglobal(L, "cjk_token_estimate");
  munit_assert_int(lua_tointeger(L, -1), ==, 3);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_compact_skips_weak_model_with_smaller_context(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  clear_messages();
  set_capstan_provider_config(L);
  set_capstan_config_weak_model(L, "openrouter", "weak/model");
  int rc = luaL_dostring(
      L,
      "capstan.config.providers.openrouter.models = {"
      "{id = 'weak/model', context_length = 1000}"
      "}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  char *history = malloc(12001);
  munit_assert_not_null(history);
  memset(history, 'h', 12000);
  history[12000] = '\0';
  add_message(history, history, MSG_USER);

  agent_compact(L);
  munit_assert_true(agent_is_running());
  munit_assert_true(strstr(captured_body, "\"model\":\"config/model\"") !=
                    NULL);
  munit_assert_true(strstr(captured_logs,
                           "[compact] weak_model_skipped") != NULL);
  send_text_done(L, "short handoff");
  munit_assert_false(agent_is_running());

  clear_messages();
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_auto_compact_skips_weak_model_with_unknown_context(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  clear_messages();
  set_capstan_provider_config(L);
  set_capstan_config_weak_model(L, "openrouter", "unknown/weak");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  char *history = malloc(strlen("history for automatic compact") + 1);
  munit_assert_not_null(history);
  strcpy(history, "history for automatic compact");
  add_message(history, history, MSG_USER);

  agent_auto_compact(L);
  munit_assert_true(agent_is_running());
  munit_assert_true(strstr(captured_body, "\"model\":\"config/model\"") !=
                    NULL);
  munit_assert_true(strstr(
      captured_logs,
      "[compact] weak_model_skipped context_limit=unknown") != NULL);
  send_text_done(L, "automatic handoff");
  munit_assert_false(agent_is_running());

  clear_messages();
  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_file_read_schema_requires_known_argument_without_composition(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  load_real_file_plugin(L);

  lua_getglobal(L, "plugins");
  lua_getfield(L, -1, "file");
  lua_getfield(L, -1, "tool");
  lua_getfield(L, -1, "parameters");

  lua_getfield(L, -1, "minProperties");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getfield(L, -1, "additionalProperties");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "anyOf");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "oneOf");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "allOf");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 5);

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
  lua_pop(L, 2);
  lua_getfield(L, -1, "reasoning_efforts");
  munit_assert_true(lua_istable(L, -1));
  munit_assert_int((int)lua_rawlen(L, -1), ==, 3);
  lua_rawgeti(L, -1, 2);
  munit_assert_string_equal(lua_tostring(L, -1), "medium");
  lua_pop(L, 3);

  lua_rawgeti(L, -1, 2);
  lua_getfield(L, -1, "id");
  lua_getfield(L, -2, "text");
  munit_assert_string_equal(lua_tostring(L, -2), "~model/z-latest");
  munit_assert_string_equal(lua_tostring(L, -1),
                            "~model/z-latest  Model Z Latest");
  lua_pop(L, 3);

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
      "default_reasoning_efforts = {'low', 'high'},"
      "models = {{id = 'static/model-a', context_limit = 1234},"
      "{id = 'static/model-no-reasoning', supported_parameters = {'temperature'}},"
      "{id = 'static/model-reasoning', supported_parameters = {'reasoning_effort'}}}"
      "}, unavailable_provider = {"
      "model = 'configured/fallback'"
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

  int found_static = 0;
  int found_static_efforts = 0;
  int found_no_reasoning = 0;
  int found_reasoning = 0;
  int found_fallback = 0;
  for (int i = 1; i <= (int)lua_rawlen(L, -1); i++) {
    lua_rawgeti(L, -1, i);
    lua_getfield(L, -1, "provider");
    lua_getfield(L, -2, "id");
    const char *provider = lua_tostring(L, -2);
    const char *id = lua_tostring(L, -1);
    if (provider && id && strcmp(provider, "static_provider") == 0 &&
        strcmp(id, "static/model-a") == 0) {
      found_static = 1;
      lua_getfield(L, -3, "reasoning_efforts");
      found_static_efforts = lua_istable(L, -1) && lua_rawlen(L, -1) == 2;
      lua_pop(L, 1);
    }
    if (provider && id && strcmp(provider, "static_provider") == 0 &&
        strcmp(id, "static/model-no-reasoning") == 0) {
      lua_getfield(L, -3, "reasoning_efforts");
      found_no_reasoning = lua_isnil(L, -1);
      lua_pop(L, 1);
    }
    if (provider && id && strcmp(provider, "static_provider") == 0 &&
        strcmp(id, "static/model-reasoning") == 0) {
      lua_getfield(L, -3, "reasoning_efforts");
      found_reasoning = lua_istable(L, -1) && lua_rawlen(L, -1) == 2;
      lua_pop(L, 1);
    }
    if (provider && id && strcmp(provider, "unavailable_provider") == 0 &&
        strcmp(id, "configured/fallback") == 0)
      found_fallback = 1;
    lua_pop(L, 3);
  }
  munit_assert_true(found_static);
  munit_assert_true(found_static_efforts);
  munit_assert_true(found_no_reasoning);
  munit_assert_true(found_reasoning);
  munit_assert_true(found_fallback);

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

static MunitResult test_plugin_tools_collection_and_permission_false(
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
      "plugins.multi = {\n"
      "  tools = {\n"
      "    { name = 'multi_regular', description = 'regular', parameters = { type = 'object', properties = { path = { type = 'string' } }, required = {'path'} } },\n"
      "    { name = 'multi_internal', permission = false, description = 'internal', parameters = { type = 'object', properties = { path = { type = 'string' } }, required = {'path'} } },\n"
      "  },\n"
      "  handler = function(ctx)\n"
      "    _G.multi_tool_name = ctx.tool_name\n"
      "    _G.multi_tool_path = ctx.tool_args.path\n"
      "    if ctx.tool_args.path == '/missing' then return ctx:error('missing ui', 'missing file') end\n"
      "    return ctx:replace('ui', 'llm:' .. ctx.tool_name .. ':' .. ctx.tool_args.path)\n"
      "  end,\n"
      "}\n"
      "local available = tools.collect({ disable_subagents = true })\n"
      "_G.multi_tool_count = 0\n"
      "for _, tool in ipairs(available) do\n"
      "  local name = tool['function'] and tool['function'].name\n"
      "  if name == 'multi_regular' or name == 'multi_internal' then _G.multi_tool_count = _G.multi_tool_count + 1 end\n"
      "end\n"
      "tools.handle_tool_calls({}, available, {{ id = 'call_multi', name = 'multi_internal', arguments = '{\"path\":\"/outside\"}' }}, '', function(msgs) _G.multi_result = msgs[#msgs].content end, { tools = available, silent_tools = true })\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "multi_tool_count");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 2);
  lua_pop(L, 1);
  lua_getglobal(L, "multi_tool_name");
  munit_assert_string_equal(lua_tostring(L, -1), "multi_internal");
  lua_pop(L, 1);
  lua_getglobal(L, "multi_result");
  munit_assert_string_equal(lua_tostring(L, -1), "llm:multi_internal:/outside");
  lua_pop(L, 1);
  munit_assert_int(permit_check_calls, ==, 0);
  munit_assert_int(permit_prompt_calls, ==, 0);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect({ disable_subagents = true })\n"
      "tools.handle_tool_calls({}, available, {{ id = 'call_missing', name = 'multi_internal', arguments = '{\"path\":\"/missing\"}' }}, '', function(msgs) _G.multi_error_result = msgs[#msgs].content end, { tools = available, silent_tools = true })\n");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "multi_error_result");
  munit_assert_string_equal(lua_tostring(L, -1), "missing file");
  lua_pop(L, 1);
  munit_assert_true(strstr(captured_logs, "[tool] error name=multi_internal") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_permission_target_uses_path(
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
      "plugins.wiki_tool_test = {\n"
      "  tools = {{ name = 'wiki_ingest', permission = 'file_read', description = 'ingest', parameters = { type = 'object', properties = { path = { type = 'string' } }, required = {'path'} } }},\n"
      "  handler = function(ctx) _G.wiki_ingest_called = true return ctx:replace('ui', 'llm') end,\n"
      "}\n"
      "local available = tools.collect({ disable_subagents = true })\n"
      "tools.handle_tool_calls({}, available, {{ id = 'call_wiki_ingest', name = 'wiki_ingest', arguments = '{\"path\":\"/tmp/source-notes\"}' }}, '', function(msgs) _G.wiki_ingest_result = msgs[#msgs].content end, { tools = available, silent_tools = true })\n");
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target, "/tmp/source-notes");
  munit_assert_int(permit_check_calls, ==, 1);
  lua_getglobal(L, "wiki_ingest_called");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "wiki_ingest_result");
  munit_assert_true(strstr(lua_tostring(L, -1), "Permission denied for wiki_ingest /tmp/source-notes") != NULL);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_yes_persists_file_read_permission(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_permit_prompt_decision("allow");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "plugins.wiki_tool_test = {\n"
      "  tools = {{ name = 'wiki_ingest', permission = 'file_read', description = 'ingest', parameters = { type = 'object', properties = { path = { type = 'string' } }, required = {'path'} } }},\n"
      "  handler = function(ctx) _G.wiki_ingest_called = true return ctx:replace('ui', 'llm') end,\n"
      "}\n"
      "local available = tools.collect({ disable_subagents = true })\n"
      "tools.handle_tool_calls({}, available, {{ id = 'call_wiki_ingest', name = 'wiki_ingest', arguments = '{\"path\":\"/tmp/source-notes\"}' }}, '', function(msgs) _G.wiki_ingest_result = msgs[#msgs].content end, { tools = available, silent_tools = true })\n");
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_int(permit_save_calls, ==, 1);
  munit_assert_int(grant_allow, ==, 1);
  munit_assert_string_equal(granted_tool, "file_read");
  munit_assert_string_equal(granted_pattern, "/tmp/source-notes");
  lua_getglobal(L, "wiki_ingest_called");
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "wiki_ingest_result");
  munit_assert_string_equal(lua_tostring(L, -1), "llm");
  lua_pop(L, 1);

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

static MunitResult test_logs_empty_stream_completion(
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
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_true(strstr(captured_logs, "[stream] empty_response") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] stream completed with no text and no tool calls") != NULL);
  munit_assert_int(post_stream_calls, ==, 2);
  munit_assert_true(strstr(captured_logs,
                           "requesting finalization attempt=1/1") != NULL);
  munit_assert_true(strstr(captured_body, "return an empty response") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "Finalizing response") != NULL);

  send_text_done(L, "finalized response");
  munit_assert_true(strstr(captured_agent_appends, "finalized response") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "[error:") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_repeated_empty_terminal_response_fails_visibly(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  for (int i = 0; i < 2; i++) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
    lua_pushnil(L);
    lua_pushboolean(L, 1);
    rc = lua_pcall(L, 2, 0, 0);
    munit_assert_int(rc, ==, LUA_OK);
  }

  munit_assert_int(post_stream_calls, ==, 2);
  munit_assert_true(strstr(captured_logs, "finalization failed") != NULL);
  munit_assert_true(strstr(captured_agent_appends,
                           "Provider returned an empty terminal response twice") != NULL);

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

  munit_assert_true(strstr(captured_logs,
                           "[stream] transport_error status=HTTP 401 detail=bad key") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] stream failed error=HTTP 401: bad key") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_stream_error_empty_body_includes_request_id(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  lua_pushstring(L, "HTTP 400");
  lua_pushnil(L);
  lua_newtable(L);
  lua_pushstring(L, "req-test-400");
  lua_setfield(L, -2, "x-request-id");
  rc = lua_pcall(L, 5, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_true(strstr(
      captured_logs,
      "[agent] stream failed error=HTTP 400: provider returned an empty error "
      "body (request id: req-test-400)") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_transient_stream_error_retries_before_output(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(post_stream_calls, ==, 1);
  munit_assert_int64(captured_stream_timeout_ms, ==, 300000);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  lua_pushstring(L, "Connection error: Timeout was reached");
  rc = lua_pcall(L, 3, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(post_stream_calls, ==, 2);
  munit_assert_true(strstr(captured_logs,
                           "stream failed before output; retrying") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_vcs_unborn_diff_is_complete_and_disables_git_extensions(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "package.loaded['agent.vcs'] = nil\n"
      "package.loaded['agent.state'] = {\n"
      "  vcs_for_workspace = function() return nil end,\n"
      "  set_vcs_for_workspace = function() return true end,\n"
      "}\n"
      "package.loaded['agent.workspace'] = {\n"
      "  configured_workspace_root = function() return '/repo' end,\n"
      "  real_workspace = function() return '/repo' end,\n"
      "  normalize_path = function(path) return '/repo/' .. path end,\n"
      "  realpath = function(path) return path end,\n"
      "  path_is_within = function() return true end,\n"
      "}\n"
      "capstan.workspace_root = '/repo'\n"
      "local calls = {}\n"
      "tools = {exec = function(argv)\n"
      "  table.insert(calls, argv)\n"
      "  if #calls == 1 then return {exit = 1, stdout = '', stderr = ''} end\n"
      "  if #calls == 2 then return {exit = 0, stdout = 'staged\\n', stderr = ''} end\n"
      "  return {exit = 0, stdout = 'worktree\\n', stderr = ''}\n"
      "end}\n"
      "local vcs = require('agent.vcs')\n"
      "local result, err = vcs.run('diff')\n"
      "assert(result and not err and result.output == 'staged\\nworktree\\n')\n"
      "assert(#calls == 3)\n"
      "for _, argv in ipairs(calls) do\n"
      "  assert(argv[2] == '--no-optional-locks')\n"
      "  assert(argv[3] == '-c' and argv[4] == 'core.fsmonitor=false')\n"
      "end\n"
      "assert(table.concat(calls[2], ' '):find('--no-ext-diff', 1, true))\n"
      "assert(table.concat(calls[2], ' '):find('--no-textconv', 1, true))\n"
      "assert(table.concat(calls[3], ' '):find('--no-ext-diff', 1, true))\n"
      "assert(table.concat(calls[3], ' '):find('--no-textconv', 1, true))\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_vcs_permission_target_uses_workspace_root(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_workdir(L, "/tmp/workspace/subdir");
  set_capstan_workspace_root(L, "/tmp/workspace");

  int rc = luaL_dostring(
      L, "plugins.vcs = assert(loadfile('plugins/vcs.lua'))()");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  send_tool_call(L, "call_vcs", "vcs",
                 "{\\\"operation\\\":\\\"diff\\\",\\\"path\\\":\\\"src/main.c\\\"}");

  munit_assert_string_equal(captured_permit_tool, "file_read");
  munit_assert_string_equal(captured_permit_target,
                            "/tmp/workspace/src/main.c");

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

static MunitResult test_file_read_inside_wiki_routes_without_permission(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_workdir(L, "/repo/project");
  set_capstan_wiki_path(L, "/tmp/capstan-internal-wiki");

  int rc = luaL_dostring(
      L,
      "plugins.wiki = {"
      " tool = {name = 'wiki_read', permission = false, description = 'Read wiki',"
      " parameters = {type = 'object', properties = {path = {type = 'string'}}, required = {'path'}}},"
      " handler = function(ctx) _G.wiki_routed_path = ctx.tool_args.path return ctx:replace('wiki ui', 'wiki llm') end"
      "}\n"
      "local tools = require('agent.tools')\n"
      "local available = tools.collect({disable_subagents = true})\n"
      "tools.handle_tool_calls({}, available, {{id = 'wiki-file-read', name = 'file_read', arguments = '{\\\"path\\\":\\\"/tmp/capstan-internal-wiki/WIKI.md\\\"}'}}, '', function(msgs) _G.wiki_routed_result = msgs[#msgs].content end, {tools = available, silent_tools = true, callbacks = {on_tool_start = function(tc) _G.wiki_trace_start = tc.name; _G.wiki_trace_path = tc.effective_arguments.path end, on_tool_done = function(tc) _G.wiki_trace_done = tc.name end}})\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(permit_check_calls, ==, 0);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_true(strstr(captured_logs, "[tool] call name=wiki_read target=WIKI.md") != NULL);

  lua_getglobal(L, "wiki_routed_path");
  munit_assert_string_equal(lua_tostring(L, -1), "WIKI.md");
  lua_pop(L, 1);
  lua_getglobal(L, "wiki_routed_result");
  munit_assert_string_equal(lua_tostring(L, -1), "wiki llm");
  lua_pop(L, 1);
  lua_getglobal(L, "wiki_trace_start");
  munit_assert_string_equal(lua_tostring(L, -1), "wiki_read");
  lua_pop(L, 1);
  lua_getglobal(L, "wiki_trace_done");
  munit_assert_string_equal(lua_tostring(L, -1), "wiki_read");
  lua_pop(L, 1);
  lua_getglobal(L, "wiki_trace_path");
  munit_assert_string_equal(lua_tostring(L, -1), "WIKI.md");
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_observer_failure_stops_before_execution(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "tools.handle_tool_calls({}, {}, {{id = 'observer-call', "
      "name = 'missing_tool', arguments = '{}'}}, '', "
      "function() tool_observer_continued = true end, {"
      "callbacks = {on_tool_start = function() error('observer boom') end}, "
      "stop_run = function(message) tool_observer_stopped = message end})");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "tool_observer_stopped");
  munit_assert_true(lua_isstring(L, -1));
  munit_assert_not_null(strstr(lua_tostring(L, -1),
                               "on_tool_start failed"));
  lua_pop(L, 1);
  lua_getglobal(L, "tool_observer_continued");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_permission_error_finishes_tool_event(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  install_mock_now_ms(L, 100);
  set_permit_decision("ask");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local combined = tools.collect({disable_subagents = true})\n"
      "local ok, err = pcall(function()\n"
      "  tools.handle_tool_calls({}, combined, {{id = 'permission-error', "
      "name = 'fetch', arguments = '{\\\"url\\\":\\\"https://example.com\\\"}'}}, '', "
      "function() permission_error_continued = true end, {callbacks = {\n"
      "    on_tool_start = function() permission_error_starts = (permission_error_starts or 0) + 1 end,\n"
      "    on_tool_done = function(_, result, tool_ok, _, wait_ms)\n"
      "      permission_error_finishes = (permission_error_finishes or 0) + 1\n"
      "      permission_error_result = result\n"
      "      permission_error_tool_ok = tool_ok\n"
      "      permission_error_wait_ms = wait_ms\n"
      "    end,\n"
      "    on_permission_request = function() MOCK_NOW_MS = MOCK_NOW_MS + 25; error('permission boom') end,\n"
      "  }})\n"
      "end)\n"
      "permission_error_call_ok = ok\n"
      "permission_error_call_error = err\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "permission_error_call_ok");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_call_error");
  munit_assert_not_null(strstr(lua_tostring(L, -1), "permission boom"));
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_starts");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_finishes");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_tool_ok");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_wait_ms");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 25);
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_result");
  munit_assert_not_null(strstr(lua_tostring(L, -1), "permission boom"));
  lua_pop(L, 1);
  lua_getglobal(L, "permission_error_continued");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_execution_error_finishes_tool_event(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  install_mock_now_ms(L, 100);
  set_permit_decision("allow");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local combined = tools.collect({disable_subagents = true})\n"
      "agent.set_activity = function() error('execution boom') end\n"
      "local ok, err = pcall(function()\n"
      "  tools.handle_tool_calls({}, combined, {{id = 'execution-error', "
      "name = 'fetch', arguments = '{\\\"url\\\":\\\"https://example.com\\\"}'}}, '', "
      "function() execution_error_continued = true end, {callbacks = {\n"
      "    on_tool_start = function() execution_error_starts = (execution_error_starts or 0) + 1 end,\n"
      "    on_tool_done = function(_, result, tool_ok)\n"
      "      execution_error_finishes = (execution_error_finishes or 0) + 1\n"
      "      execution_error_result = result\n"
      "      execution_error_tool_ok = tool_ok\n"
      "    end,\n"
      "  }})\n"
      "end)\n"
      "execution_error_call_ok = ok\n"
      "execution_error_call_error = err\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "execution_error_call_ok");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "execution_error_call_error");
  munit_assert_not_null(strstr(lua_tostring(L, -1), "execution boom"));
  lua_pop(L, 1);
  lua_getglobal(L, "execution_error_starts");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "execution_error_finishes");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "execution_error_tool_ok");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "execution_error_result");
  munit_assert_not_null(strstr(lua_tostring(L, -1), "execution boom"));
  lua_pop(L, 1);
  lua_getglobal(L, "execution_error_continued");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_observer_preserves_raw_and_effective_arguments(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local combined = tools.collect({disable_subagents = true})\n"
      "tools.handle_tool_calls({}, combined, {{id = 'observer-call', "
      "name = 'fetch', arguments = '{\\\"url\\\":\\\"https://example.com\\\"}'}}, '', "
      "function() tool_observer_continued = true end, {"
      "callbacks = {on_tool_start = function(call) "
      "tool_observer_raw_type = type(call.arguments); "
      "tool_observer_original = call.original_arguments; "
      "tool_observer_effective_type = type(call.effective_arguments); "
      "tool_observer_effective_url = call.effective_arguments.url end}})");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "tool_observer_raw_type");
  munit_assert_string_equal(lua_tostring(L, -1), "string");
  lua_pop(L, 1);
  lua_getglobal(L, "tool_observer_original");
  munit_assert_string_equal(lua_tostring(L, -1),
                            "{\"url\":\"https://example.com\"}");
  lua_pop(L, 1);
  lua_getglobal(L, "tool_observer_effective_type");
  munit_assert_string_equal(lua_tostring(L, -1), "table");
  lua_pop(L, 1);
  lua_getglobal(L, "tool_observer_effective_url");
  munit_assert_string_equal(lua_tostring(L, -1), "https://example.com");
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_no_wiki_hides_tools_and_disables_configured_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_wiki_path(L, "/tmp/capstan-internal-wiki");

  int rc = luaL_dostring(
      L,
      "capstan.runtime_options = {disable_wiki = true}\n"
      "plugins.wiki = {id = 'wiki', tools = {"
      "{name = 'wiki_read', permission = false, description = 'Read wiki', parameters = {type = 'object'}},"
      "{name = 'wiki_source_read', permission = false, description = 'Read source', parameters = {type = 'object'}}"
      "}}\n"
      "local workspace = require('agent.workspace')\n"
      "local tools = require('agent.tools')\n"
      "no_wiki_path = workspace.configured_wiki_path()\n"
      "no_wiki_relative = workspace.wiki_relative_path('/tmp/capstan-internal-wiki/WIKI.md')\n"
      "no_wiki_tools = tools.names(tools.collect({disable_subagents = true}))\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "no_wiki_path");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "no_wiki_relative");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "no_wiki_tools");
  const char *names = lua_tostring(L, -1);
  munit_assert_not_null(names);
  munit_assert_true(strstr(names, "wiki_read") == NULL);
  munit_assert_true(strstr(names, "wiki_source_read") == NULL);
  munit_assert_true(strstr(names, "file_read") != NULL);
  lua_pop(L, 1);

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

static MunitResult test_explicit_allow_reads_sensitive_file_without_prompt(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  permit_explicit_allow = 1;
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
                 "\"id\":\"call_explicit_env\",\"function\":{\"name\":\"file_read\","
                 "\"arguments\":\"{\\\"path\\\":\\\".env.example\\\"}\"}}]}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_string_equal(captured_permit_target, "/repo/project/.env.example");

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
                 "data: {\"choices\":[{\"delta\":{\"content\":\"Checking first.\"}}]}\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

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
  munit_assert_true(strstr(captured_logs,
                           "[stream] mixed_text_and_tool_calls text_bytes=15 final_tool_calls=1") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] continuing mixed response text_bytes=15 tool_calls=1") != NULL);
  munit_assert_true(strstr(captured_agent_appends,
                           "Checking first.\n\n⚙ fetch") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tools] received 1 tool call(s) assistant_text_bytes=15") != NULL);
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
  munit_assert_int(setenv("CAPSTAN_LOG_LEVEL", "debug", 1), ==, 0);

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
  unsetenv("CAPSTAN_LOG_LEVEL");
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

  int rc = luaL_dostring(
      L, "capstan.config = {agent = {completion_review = true}}");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
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

  send_text_delta(L, "Running the focused validation before finalizing.");
  munit_assert_true(strstr(captured_agent_appends,
                           "Running the focused validation") == NULL);
  send_tool_call(L, "call_validate", "shell",
                 "{\\\"command\\\":\\\"npm test\\\"}");
  munit_assert_true(strstr(captured_agent_appends,
                           "Running the focused validation") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "Validating") != NULL);
  send_text_done(L, "draft answer");
  munit_assert_true(strstr(captured_body, "bounded completion review") == NULL);
  munit_assert_true(strstr(captured_agent_appends, "draft answer") != NULL);
  munit_assert_true(strstr(captured_logs, "completion_review started") == NULL);

  reset_captures(L);
  lua_close(L);
  unlink(path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_unvalidated_multi_file_write_starts_completion_review(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-multi-edit");
  char first_path[4096];
  char second_path[4096];
  snprintf(first_path, sizeof(first_path), "%s/one.txt", workdir);
  snprintf(second_path, sizeof(second_path), "%s/two.txt", workdir);
  write_file(first_path, "old-one\n");
  write_file(second_path, "old-two\n");

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dostring(
      L, "capstan.config = {agent = {completion_review = true}}");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_edit_plugin(L);

  call_agent_entry(L);
  send_tool_call(L, "write-one", "file_edit",
                 "{\\\"path\\\":\\\"one.txt\\\",\\\"old_text\\\":\\\"old-one\\\",\\\"new_text\\\":\\\"new-one\\\"}");
  send_tool_call(L, "write-two", "file_edit",
                 "{\\\"path\\\":\\\"two.txt\\\",\\\"old_text\\\":\\\"old-two\\\",\\\"new_text\\\":\\\"new-two\\\"}");
  send_text_done(L, "draft answer");

  munit_assert_true(strstr(captured_body, "bounded completion review") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "draft answer") == NULL);
  munit_assert_true(strstr(captured_logs, "completion_review started") != NULL);

  send_text_done(L, "reviewed answer");
  munit_assert_true(strstr(captured_agent_appends, "reviewed answer") != NULL);

  reset_captures(L);
  lua_close(L);
  unlink(first_path);
  unlink(second_path);
  rmdir(workdir);
  return MUNIT_OK;
}

static MunitResult test_write_after_validation_restores_completion_review(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-post-validation-edit");
  char first_path[4096];
  char second_path[4096];
  snprintf(first_path, sizeof(first_path), "%s/one.txt", workdir);
  snprintf(second_path, sizeof(second_path), "%s/two.txt", workdir);
  write_file(first_path, "old-one\n");
  write_file(second_path, "old-two\n");

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dostring(
      L, "capstan.config = {agent = {completion_review = true}}");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_edit_plugin(L);

  call_agent_entry(L);
  send_tool_call(L, "write-one", "file_edit",
                 "{\\\"path\\\":\\\"one.txt\\\",\\\"old_text\\\":\\\"old-one\\\",\\\"new_text\\\":\\\"new-one\\\"}");
  send_tool_call(L, "validate", "shell",
                 "{\\\"command\\\":\\\"npm test\\\"}");
  send_tool_call(L, "write-two", "file_edit",
                 "{\\\"path\\\":\\\"two.txt\\\",\\\"old_text\\\":\\\"old-two\\\",\\\"new_text\\\":\\\"new-two\\\"}");
  send_text_done(L, "draft answer");

  munit_assert_true(strstr(captured_body, "bounded completion review") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "draft answer") == NULL);

  send_text_done(L, "reviewed answer");
  munit_assert_true(strstr(captured_agent_appends, "reviewed answer") != NULL);

  reset_captures(L);
  lua_close(L);
  unlink(first_path);
  unlink(second_path);
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
  munit_assert_true(strstr(captured_body, "\"paths\"") != NULL);
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

static MunitResult test_file_read_image_becomes_multimodal_message(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  munit_assert_not_null(getcwd(workdir, sizeof(workdir)));
  const char *fixture = "test/fixtures/vision-shapes.png";
  struct stat st;
  munit_assert_int(stat(fixture, &st), ==, 0);
  munit_assert_size((size_t)st.st_size, >, 1000);

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_plugin(L);

  call_agent_entry(L);
  send_tool_call(L, "call_file_image", "file_read",
                 "{\\\"path\\\":\\\"test/fixtures/vision-shapes.png\\\"}");

  lua_pushlstring(L, captured_body, strlen(captured_body));
  lua_setglobal(L, "CAPTURED_REQUEST_BODY");
  rc = luaL_dostring(
      L,
      "local json = require('vendor.rxi.json')\n"
      "local request = json.decode(CAPTURED_REQUEST_BODY)\n"
      "local tool_index\n"
      "for i, message in ipairs(request.messages or {}) do\n"
      "  if message.role == 'tool' and "
      "message.tool_call_id == 'call_file_image' then\n"
      "    tool_index = i\n"
      "    break\n"
      "  end\n"
      "end\n"
      "local vision = tool_index and request.messages[tool_index + 1]\n"
      "local content = vision and vision.content\n"
      "local text = type(content) == 'table' and content[1]\n"
      "local image = type(content) == 'table' and content[2]\n"
      "local image_url = image and image.image_url\n"
      "VISION_REQUEST_OK = vision and vision.role == 'user' and\n"
      "  text and text.type == 'text' and\n"
      "  image and image.type == 'image_url' and\n"
      "  image_url and image_url.detail == 'auto' and\n"
      "  type(image_url.url) == 'string' and\n"
      "  image_url.url:match('^data:image/png;base64,iVBORw0KGgo') ~= nil\n");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "VISION_REQUEST_OK");
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  munit_assert_true(strstr(captured_logs, "iVBORw0KGgo") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_file_read_non_image_binary_is_not_inserted_as_text(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char workdir[4096];
  make_tmp_dir(workdir, sizeof(workdir), "provider-file-read-binary");
  char path[4096];
  snprintf(path, sizeof(path), "%s/data.bin", workdir);
  const unsigned char bytes[] = {0xff, 0xfe, 0x00, 0x41};
  write_file_bytes(path, bytes, sizeof(bytes));

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, workdir);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  load_real_file_plugin(L);

  call_agent_entry(L);
  send_tool_call(L, "call_file_binary", "file_read",
                 "{\\\"path\\\":\\\"data.bin\\\"}");

  munit_assert_true(strstr(captured_body, "4-byte binary file") != NULL);
  munit_assert_true(strstr(captured_body, "\"type\":\"image_url\"") == NULL);

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

static MunitResult test_tool_activity_wraps_top_level_execution(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='activity', name='fetch', arguments='{\\\"url\\\":\\\"https://example.com\\\"}'}}, '', function() end, {tools=available, silent_tools=true})\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(captured_activity_during_tool, "Fetching");
  munit_assert_string_equal(agent_activity(), "");

  captured_activity_during_tool[0] = '\0';
  rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='quiet', name='fetch', arguments='{\\\"url\\\":\\\"https://example.com\\\"}'}}, '', function() end, {tools=available, silent_tools=true, update_status=false})\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(captured_activity_during_tool, "");
  munit_assert_string_equal(agent_activity(), "");

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

static void send_reasoning_delta(lua_State *L, const char *reasoning,
                                 int include_details) {
  char event[4096];
  if (include_details) {
    snprintf(event, sizeof(event),
             "data: {\"choices\":[{\"delta\":{\"reasoning\":\"%s\","
             "\"reasoning_details\":[{\"type\":\"reasoning.text\","
             "\"text\":\"%s\",\"id\":\"reasoning-1\","
             "\"format\":\"unknown\",\"index\":0}]}}]}\n\n",
             reasoning, reasoning);
  } else {
    snprintf(event, sizeof(event),
             "data: {\"choices\":[{\"delta\":{\"reasoning\":\"%s\"}}]}\n\n",
             reasoning);
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, event);
  lua_pushboolean(L, 0);
  int rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void send_reasoning_detail_fragment(lua_State *L, const char *text,
                                           const char *signature) {
  char event[4096];
  if (signature) {
    snprintf(event, sizeof(event),
             "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{"
             "\"type\":\"reasoning.text\",\"text\":\"%s\","
             "\"signature\":\"%s\",\"id\":\"reasoning-1\","
             "\"format\":\"anthropic-claude-v1\",\"index\":0}]}}]}\n\n",
             text, signature);
  } else {
    snprintf(event, sizeof(event),
             "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{"
             "\"type\":\"reasoning.text\",\"text\":\"%s\","
             "\"id\":\"reasoning-1\","
             "\"format\":\"anthropic-claude-v1\",\"index\":0}]}}]}\n\n",
             text);
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, event);
  lua_pushboolean(L, 0);
  int rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void send_text_delta(lua_State *L, const char *text) {
  char event[2048];
  snprintf(event, sizeof(event),
           "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n",
           text);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, event);
  lua_pushboolean(L, 0);
  int rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
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

static MunitResult test_session_title_generation_is_silent(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);
  set_config_hook(L,
                  "after_agent_turn = function(ctx) "
                  "session_after_turn_calls = (session_after_turn_calls or 0) + 1 "
                  "return ctx "
                  "end");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  rc = luaL_dostring(
      L,
      "agent.session_title_context = function()\n"
      "  return 'session-1', 'How do sessions work?', 'They are persisted.'\n"
      "end\n"
      "agent.set_session_title = function(id, title)\n"
      "  generated_session_id = id\n"
      "  generated_session_title = title\n"
      "end\n");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);
  send_text_done(L, "Visible answer");
  munit_assert_int(post_stream_calls, ==, 2);
  munit_assert_true(captured_stream_background);
  munit_assert_string_equal(captured_agent_appends, "Visible answer");
  munit_assert_true(strstr(captured_body, "How do sessions work?") != NULL);
  munit_assert_true(strstr(captured_body, "They are persisted.") != NULL);
  lua_getglobal(L, "session_after_turn_calls");
  munit_assert_int(lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);

  send_text_done(L, "Persisted session workflow");
  munit_assert_string_equal(captured_agent_appends, "Visible answer");
  lua_getglobal(L, "generated_session_id");
  munit_assert_string_equal(lua_tostring(L, -1), "session-1");
  lua_pop(L, 1);
  lua_getglobal(L, "generated_session_title");
  munit_assert_string_equal(lua_tostring(L, -1),
                            "Persisted session workflow");
  lua_pop(L, 1);
  lua_getglobal(L, "session_after_turn_calls");
  munit_assert_int(lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_failed_response_skips_session_title(
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
      "agent.session_title_context = function()\n"
      "  title_context_calls = (title_context_calls or 0) + 1\n"
      "  return 'session-1', 'Question', 'Partial answer'\n"
      "end\n");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  lua_pushstring(L, "HTTP 401");
  lua_pushstring(L, "{\"error\":\"bad key\"}");
  rc = lua_pcall(L, 4, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(post_stream_calls, ==, 1);
  lua_getglobal(L, "title_context_calls");
  munit_assert_true(lua_isnil(L, -1));
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
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

static MunitResult test_tool_guard_soft_skips_redundant_generated_output_checks(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_agent_config_number(L, "max_generated_output_checks", 1);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  send_tool_call(L, "call_generated_check_1", "shell",
                 "{\\\"command\\\":\\\"grep metric-card dist/app.js\\\"}");
  int permit_calls_after_first = permit_check_calls;
  send_tool_call(L, "call_generated_check_2", "shell",
                 "{\\\"command\\\":\\\"cat dist/app.js\\\"}");

  munit_assert_int(permit_check_calls, ==, permit_calls_after_first);
  munit_assert_true(strstr(captured_body,
                           "Skipped redundant generated-output inspection") !=
                    NULL);
  munit_assert_true(strstr(captured_logs,
                           "generated_output_check_skipped") != NULL);
  munit_assert_true(strstr(captured_agent_appends, "[stopped:") == NULL);

  send_tool_call(L, "call_source_check", "shell",
                 "{\\\"command\\\":\\\"grep metric-card src/App.tsx\\\"}");
  munit_assert_int(permit_check_calls, ==, permit_calls_after_first + 1);

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

static MunitResult test_tool_guard_finishes_event_before_stopping_run(
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

  rc = luaL_dostring(
      L,
      "guard_callback_order = {}\n"
      "local function record(value) table.insert(guard_callback_order, value) end\n"
      "local ok, err = capstan.agent.run({messages = {{role = 'user', content = 'inspect'}}}, {\n"
      "  on_tool_start = function() record('tool_start') end,\n"
      "  on_tool_done = function(_, result, tool_ok)\n"
      "    assert(tool_ok == false)\n"
      "    assert(result:find('agent run exceeded 1s', 1, true))\n"
      "    record('tool_done')\n"
      "  end,\n"
      "  on_error = function() record('error') end,\n"
      "  on_done = function() record('done') end,\n"
      "})\n"
      "assert(ok, err)\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  set_mock_now_ms(L, 2000);
  send_tool_call(L, "call_guard_callback_order", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  lua_getglobal(L, "guard_callback_order");
  munit_assert_true(lua_istable(L, -1));
  munit_assert_size(lua_rawlen(L, -1), ==, 4);
  const char *expected[] = {"tool_start", "tool_done", "error", "done"};
  for (int i = 0; i < 4; i++) {
    lua_rawgeti(L, -1, i + 1);
    munit_assert_string_equal(lua_tostring(L, -1), expected[i]);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_default_duration_is_2700_seconds(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  install_mock_now_ms(L, 0);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);
  set_mock_now_ms(L, 901000);
  send_tool_call(L, "call_before_default_duration", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_true(strstr(captured_agent_appends, "[stopped:") == NULL);
  munit_assert_int(post_stream_calls, ==, 2);

  set_mock_now_ms(L, 2701000);
  send_tool_call(L, "call_after_default_duration", "fetch",
                 "{\\\"url\\\":\\\"https://example.com\\\"}");

  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: agent run exceeded 2700s") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[tool_guard] agent run exceeded 2700s") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_guard_pauses_during_permission_prompt(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_permit_prompt_decision("allow");
  set_agent_config_number(L, "max_duration_sec", 1);
  install_mock_now_ms(L, 0);
  permit_prompt_advance_ms = 5000;

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  set_mock_now_ms(L, 500);
  send_tool_call(L, "call_permission_wait", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");

  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_int(post_stream_calls, ==, 2);
  munit_assert_true(strstr(captured_agent_appends, "[stopped:") == NULL);
  munit_assert_true(strstr(captured_logs, "prompt_wait_ms=5000") != NULL);

  permit_prompt_advance_ms = 0;
  set_mock_now_ms(L, 6101);
  send_tool_call(L, "call_after_permission_wait", "shell",
                 "{\\\"command\\\":\\\"ls\\\"}");

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_true(strstr(captured_agent_appends,
                           "[stopped: agent run exceeded 1s") != NULL);

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

static MunitResult test_shell_always_allow_is_session_scoped(
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
  munit_assert_string_equal(granted_tool, "");
  munit_assert_int(permit_save_calls, ==, 0);
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
  munit_assert_int(permit_save_calls, ==, 0);
  munit_assert_true(strstr(captured_body, "shell llm: ls src") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_yolo_persists_across_interactive_sessions(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dostring(
      L,
      "TEST_SESSION_ID = 'session-a'\n"
      "agent.session_id = function() return TEST_SESSION_ID end\n");
  munit_assert_int(rc, ==, LUA_OK);
  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  rc = luaL_dostring(L, "capstan.agent.set_yolo(true)");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);
  send_tool_call(L, "call_yolo_session_a", "shell",
                 "{\\\"command\\\":\\\"pwd\\\"}");
  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);

  rc = luaL_dostring(L, "TEST_SESSION_ID = 'session-b'");
  munit_assert_int(rc, ==, LUA_OK);
  call_agent_entry(L);
  send_tool_call(L, "call_yolo_session_b", "shell",
                 "{\\\"command\\\":\\\"ls src\\\"}");

  munit_assert_int(permit_check_calls, ==, 2);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_true(strstr(captured_body, "shell llm: ls src") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_yolo_does_not_mutate_external_permission_scope(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_provider_config(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  rc = luaL_dostring(
      L,
      "local scope = {allowed_tools = {}, allowed_targets = {}, full_control = false}\n"
      "capstan.agent.set_yolo(true)\n"
      "capstan.agent.run({messages = {{role = 'user', content = 'test'}}, permission_scope = scope, update_status = false}, {})\n"
      "external_scope_yolo_after_run = scope.yolo == true\n"
      "capstan.agent.set_yolo(false)\n"
      "external_scope_yolo_after_disable = scope.yolo == true\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "external_scope_yolo_after_run");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "external_scope_yolo_after_disable");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_yolo_allows_sensitive_path_without_prompt(
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
  rc = luaL_dostring(L, "capstan.agent.set_yolo(true)");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);
  send_tool_call(L, "call_yolo_sensitive_env", "file_read",
                 "{\\\"path\\\":\\\".env.local\\\"}");

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_true(strstr(captured_body, "file llm") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_yolo_preserves_explicit_deny(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("deny");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  rc = luaL_dostring(L, "capstan.agent.set_yolo(true)");
  munit_assert_int(rc, ==, LUA_OK);

  call_agent_entry(L);
  send_tool_call(L, "call_yolo_denied_env", "file_read",
                 "{\\\"path\\\":\\\".env.local\\\"}");

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_true(strstr(captured_body, "Permission denied for file_read") != NULL);

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
  install_mock_now_ms(L, 0);

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

  rc = luaL_dostring(
      L,
      "local stream = require('agent.stream') "
      "minimax_metric_callback = stream.stream({"
      "model = 'minimax/minimax-m3', suppress_agent_state = true}, "
      "function(result, done) if done then minimax_metrics = result.metrics end end, "
      "0, {})");
  munit_assert_int(rc, ==, LUA_OK);
  set_mock_now_ms(L, 100);
  lua_getglobal(L, "minimax_metric_callback");
  lua_pushstring(L, "data: {\"choices\":[{\"delta\":{\"content\":\"early\"}}]}\n\n");
  lua_pushboolean(L, 0);
  munit_assert_int(lua_pcall(L, 2, 0, 0), ==, LUA_OK);
  set_mock_now_ms(L, 500);
  lua_getglobal(L, "minimax_metric_callback");
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  munit_assert_int(lua_pcall(L, 2, 0, 0), ==, LUA_OK);
  lua_getglobal(L, "minimax_metrics");
  munit_assert_true(lua_istable(L, -1));
  lua_getfield(L, -1, "first_output_ms");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 100);
  lua_pop(L, 1);
  lua_getfield(L, -1, "first_text_ms");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 500);
  lua_pop(L, 2);

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

  munit_assert_int(permit_check_calls, ==, 2);
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

  munit_assert_int(permit_check_calls, ==, 2);
  munit_assert_int(permit_prompt_calls, ==, 1);
  munit_assert_int(permit_save_calls, ==, 0);
  munit_assert_string_equal(granted_tool, "");
  munit_assert_true(strstr(captured_body, "\"tool_call_id\":\"call_full_run_fetch\"") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_workdir_only_full_control_allows_workspace_shell(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_benchmark_shell', name='shell', arguments='{\\\"command\\\":\\\"pwd\\\"}'}}, '', function() end, {tools = available, silent_tools = true, permission_scope = {allowed_tools = {}, full_control = true, workdir_only = true}})\n");
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_workdir_only_full_control_denies_outside_shell_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_capstan_workdir(L, "/repo/project/task");
  set_capstan_workspace_root(L, "/repo/project");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls({}, available, {{id='call_outside_shell', name='shell', arguments='{\\\"command\\\":\\\"find /Users/alxd -name two-bucket\\\"}'}}, '', function() end, {tools = available, silent_tools = true, permission_scope = {allowed_tools = {}, full_control = true, workdir_only = true}})\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(permit_check_calls, ==, 0);
  munit_assert_int(permit_prompt_calls, ==, 0);
  munit_assert_true(strstr(captured_logs, "shell path escapes workspace") != NULL);
  munit_assert_true(strstr(captured_body, "shell llm:") == NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_workdir_only_tracks_nested_shell_cd(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_capstan_workdir(L, "/repo/project");
  set_capstan_workspace_root(L, "/repo/project");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "local scope = {allowed_tools = {}, full_control = true, workdir_only = true}\n"
      "nested_cd_result = ''\n"
      "nested_escape_result = ''\n"
      "missing_cd_result = ''\n"
      "tools.handle_tool_calls({}, available, {{id='nested-cd', name='shell', arguments='{\"command\":\"mkdir -p build && cd build && cmake ..\"}'}}, '', function(msgs) nested_cd_result = msgs[#msgs].content end, {tools = available, silent_tools = true, permission_scope = scope})\n"
      "tools.handle_tool_calls({}, available, {{id='nested-escape', name='shell', arguments='{\"command\":\"cd build && cat ../../outside\"}'}}, '', function(msgs) nested_escape_result = msgs[#msgs].content end, {tools = available, silent_tools = true, permission_scope = scope})\n"
      "tools.handle_tool_calls({}, available, {{id='missing-cd', name='shell', arguments='{\"command\":\"cd && cat project.txt\"}'}}, '', function(msgs) missing_cd_result = msgs[#msgs].content end, {tools = available, silent_tools = true, permission_scope = scope})\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);

  lua_getglobal(L, "nested_cd_result");
  munit_assert_true(strstr(lua_tostring(L, -1), "shell llm:") != NULL);
  lua_pop(L, 1);
  lua_getglobal(L, "nested_escape_result");
  munit_assert_true(strstr(lua_tostring(L, -1),
                           "shell path escapes workspace: /repo/outside") != NULL);
  lua_pop(L, 1);
  lua_getglobal(L, "missing_cd_result");
  munit_assert_true(strstr(lua_tostring(L, -1),
                           "cd requires a static workspace path") != NULL);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_workdir_only_checks_leading_redirection_paths(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("ask");
  set_capstan_workdir(L, "/repo/project/task");
  set_capstan_workspace_root(L, "/repo/project");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "local scope = {allowed_tools = {}, full_control = true, workdir_only = true}\n"
      "leading_input_result = ''\n"
      "leading_output_result = ''\n"
      "leading_inside_result = ''\n"
      "tools.handle_tool_calls({}, available, {{id='leading-input', name='shell', arguments='{\"command\":\"</etc/passwd head -n1\"}'}}, '', function(msgs) leading_input_result = msgs[#msgs].content end, {tools = available, silent_tools = true, permission_scope = scope})\n"
      "tools.handle_tool_calls({}, available, {{id='leading-output', name='shell', arguments='{\"command\":\">/tmp/output echo x\"}'}}, '', function(msgs) leading_output_result = msgs[#msgs].content end, {tools = available, silent_tools = true, permission_scope = scope})\n"
      "tools.handle_tool_calls({}, available, {{id='leading-inside', name='shell', arguments='{\"command\":\">/repo/project/output echo x\"}'}}, '', function(msgs) leading_inside_result = msgs[#msgs].content end, {tools = available, silent_tools = true, permission_scope = scope})\n");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_int(permit_check_calls, ==, 1);
  munit_assert_int(permit_prompt_calls, ==, 0);

  lua_getglobal(L, "leading_input_result");
  munit_assert_true(strstr(lua_tostring(L, -1),
                           "shell path escapes workspace: /etc/passwd") != NULL);
  lua_pop(L, 1);
  lua_getglobal(L, "leading_output_result");
  munit_assert_true(strstr(lua_tostring(L, -1),
                           "shell path escapes workspace: /tmp/output") != NULL);
  lua_pop(L, 1);
  lua_getglobal(L, "leading_inside_result");
  munit_assert_true(strstr(lua_tostring(L, -1), "shell llm:") != NULL);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_failed_shell_validation_is_not_successful(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_permit_decision("allow");
  set_capstan_workdir(L, "/repo/project");

  int rc = luaL_dostring(
      L,
      "local tools = require('agent.tools')\n"
      "local available = tools.collect()\n"
      "local state = {workspace_mutated = true}\n"
      "tools.handle_tool_calls({}, available, {{id='failed-validation', name='shell', arguments='{\"command\":\"npm test --fail\"}'}}, '', function() end, {tools = available, silent_tools = true, state = state, permission_scope = {allowed_tools = {}, full_control = true}})\n"
      "failed_validation_marked = state.successful_validation == true\n"
      "tools.handle_tool_calls({}, available, {{id='passed-validation', name='shell', arguments='{\"command\":\"npm test\"}'}}, '', function() end, {tools = available, silent_tools = true, state = state, permission_scope = {allowed_tools = {}, full_control = true}})\n"
      "passed_validation_marked = state.successful_validation == true\n"
      "local direct_state = {workspace_mutated = true}\n"
      "tools.handle_tool_calls({}, available, {{id='direct-validation', name='shell', arguments='{\"command\":\"javac /tmp/Check.java && java -cp /tmp Check\"}'}}, '', function() end, {tools = available, silent_tools = true, state = direct_state, permission_scope = {allowed_tools = {}, full_control = true}})\n"
      "direct_validation_marked = direct_state.successful_validation == true\n"
      "local cmake_state = {workspace_mutated = true}\n"
      "tools.handle_tool_calls({}, available, {{id='cmake-validation', name='shell', arguments='{\"command\":\"cmake --build build -j2\"}'}}, '', function() end, {tools = available, silent_tools = true, state = cmake_state, permission_scope = {allowed_tools = {}, full_control = true}})\n"
      "cmake_validation_marked = cmake_state.successful_validation == true\n"
      "local search_state = {workspace_mutated = true}\n"
      "tools.handle_tool_calls({}, available, {{id='search-not-validation', name='shell', arguments='{\"command\":\"rg test src\"}'}}, '', function() end, {tools = available, silent_tools = true, state = search_state, permission_scope = {allowed_tools = {}, full_control = true}})\n"
      "search_validation_marked = search_state.successful_validation == true\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "failed_validation_marked");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "passed_validation_marked");
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "direct_validation_marked");
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "cmake_validation_marked");
  munit_assert_true(lua_toboolean(L, -1));
  lua_pop(L, 1);
  lua_getglobal(L, "search_validation_marked");
  munit_assert_false(lua_toboolean(L, -1));
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_request_includes_workdir_and_workspace_root(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  set_capstan_workdir(L, "/repo/project/task");
  set_capstan_workspace_root(L, "/repo/project");
  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_true(strstr(captured_body,
                           "Working directory: /repo/project/task") != NULL);
  munit_assert_true(strstr(captured_body,
                           "Workspace root: /repo/project") != NULL);

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
      "SUBAGENT_PERMISSION_CALLBACKS = 0\n"
      "local permission_scope = {allowed_tools = {}, full_control = false}\n"
      "local parent_callbacks = {on_permission_request = function()\n"
      "  SUBAGENT_PERMISSION_CALLBACKS = SUBAGENT_PERMISSION_CALLBACKS + 1\n"
      "  return 'allow_tool_run'\n"
      "end}\n"
      "capstan.agent.run = function(opts, callbacks)\n"
      "  tools.handle_tool_calls({}, opts.tools, {{id='inner_' .. opts.messages[1].content, name='shell', arguments='{\\\"command\\\":\\\"pwd\\\"}'}}, '', function() end, {tools = opts.tools, depth = 1, permission_scope = opts.permission_scope, silent_tools = true, callbacks = callbacks})\n"
      "  callbacks.on_done({ok = true, text = 'done', turns = 1})\n"
      "  return true, nil\n"
      "end\n"
      "local current_msgs = {}\n"
      "local available = tools.collect()\n"
      "tools.handle_tool_calls(current_msgs, available, {{id='call_subs_scope', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"one\\\",\\\"task\\\":\\\"one\\\"},{\\\"id\\\":\\\"two\\\",\\\"task\\\":\\\"two\\\"}],\\\"max_concurrent\\\":2}'}}, '', function() end, {tools = available, depth = 0, permission_scope = permission_scope, callbacks = parent_callbacks})\n"
      "tools.handle_tool_calls(current_msgs, available, {{id='call_subs_scope_again', name='subagents', arguments='{\\\"tasks\\\":[{\\\"id\\\":\\\"three\\\",\\\"task\\\":\\\"three\\\"}]}'}}, '', function() end, {tools = available, depth = 0, permission_scope = permission_scope, callbacks = parent_callbacks})\n");
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(permit_check_calls, ==, 3);
  munit_assert_int(permit_prompt_calls, ==, 0);
  lua_getglobal(L, "SUBAGENT_PERMISSION_CALLBACKS");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);
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

static MunitResult test_isolated_runtime_ignores_config_agent_and_hooks(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  int rc = luaL_dostring(
      L,
      "capstan.config = capstan.config or {} "
      "capstan.config.agent = {reasoning_effort = 'high'} "
      "capstan.config.hooks = {before_request = function(ctx) "
      "ctx.request.metadata = {tag = 'must-not-run'} return ctx end} "
      "capstan.runtime_options = {isolated = true, disable_mcp = true, "
      "disable_wiki = true}");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  call_agent_entry(L);

  munit_assert_true(strstr(captured_body, "must-not-run") == NULL);
  munit_assert_true(strstr(captured_body,
                           "\"reasoning_effort\":\"medium\"") != NULL);
  munit_assert_true(strstr(captured_body,
                           "\"reasoning_effort\":\"high\"") == NULL);

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

static MunitResult test_stream_preserves_split_crlf_event(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  int rc = luaL_dostring(
      L,
      "local stream = require('agent.stream') "
      "split_crlf_text = '' "
      "split_crlf_callback = stream.stream({"
      "suppress_agent_state = true, "
      "parse_sse_event = function(raw) "
      "return {type = 'text', content = raw} end}, "
      "function(result) "
      "if result.type == 'text' then "
      "split_crlf_text = split_crlf_text .. result.content end "
      "end, 0, {})");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "split_crlf_callback");
  lua_pushstring(L, "first\r");
  lua_pushboolean(L, 0);
  munit_assert_int(lua_pcall(L, 2, 0, 0), ==, LUA_OK);

  lua_getglobal(L, "split_crlf_callback");
  lua_pushstring(L, "\nsecond\r\n\r\n");
  lua_pushboolean(L, 0);
  munit_assert_int(lua_pcall(L, 2, 0, 0), ==, LUA_OK);

  lua_getglobal(L, "split_crlf_text");
  munit_assert_string_equal(lua_tostring(L, -1), "first\nsecond");
  lua_pop(L, 1);

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

static MunitResult test_json_array_preserves_empty_array_encoding(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  int rc = luaL_dostring(
      L,
      "local json = require('vendor.rxi.json')\n"
      "encoded_empty_array = json.encode(json.array({}))\n"
      "encoded_decoded_array = json.encode(json.decode('[]'))\n");
  munit_assert_int(rc, ==, LUA_OK);

  lua_getglobal(L, "encoded_empty_array");
  munit_assert_string_equal(lua_tostring(L, -1), "[]");
  lua_pop(L, 1);
  lua_getglobal(L, "encoded_decoded_array");
  munit_assert_string_equal(lua_tostring(L, -1), "[]");
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_runtime_sanitizes_invalid_utf8_before_request(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  lua_getglobal(L, "agent_entry");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "user");
  lua_setfield(L, -2, "role");
  const char invalid[] = {'b', 'a', 'd', ':', (char)0xd0, '.', '\0'};
  lua_pushlstring(L, invalid, sizeof(invalid) - 1);
  lua_setfield(L, -2, "content");
  lua_rawseti(L, -2, 1);
  rc = lua_pcall(L, 1, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_not_null(strstr(captured_body, "bad:\xef\xbf\xbd."));
  munit_assert_true(strstr(captured_logs,
                           "[api] replaced_invalid_utf8_bytes=1") != NULL);

  rc = luaL_dostring(
      L,
      "local utf8 = require('agent.utf8')\n"
      "local input = {['bad' .. string.char(0xd0)] = 'value'}\n"
      "local sanitized, count = utf8.sanitize_values(input)\n"
      "sanitized_key, sanitized_key_count = next(sanitized), count\n");
  munit_assert_int(rc, ==, LUA_OK);
  lua_getglobal(L, "sanitized_key");
  munit_assert_string_equal(lua_tostring(L, -1), "bad\xef\xbf\xbd");
  lua_pop(L, 1);
  lua_getglobal(L, "sanitized_key_count");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 1);
  lua_pop(L, 1);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_tool_output_is_bounded_before_continuation(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  lua_State *L = new_provider_state();
  reset_captures(L);

  int rc = luaL_dostring(
      L,
      "plugins.large_output = {\n"
      "  tool = {\n"
      "    name = 'large_output', permission = false,\n"
      "    description = 'Return a large result',\n"
      "    parameters = {type = 'object', properties = {}}\n"
      "  },\n"
      "  handler = function() return string.rep('x', 60000) end\n"
      "}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);
  call_agent_entry(L);
  send_tool_call(L, "call_large", "large_output", "{}");

  munit_assert_not_null(strstr(
      captured_body,
      "[Tool output truncated: 60000 bytes; use a narrower query or a paged "
      "read.]"));
  munit_assert_true(strlen(captured_body) < 60000);
  munit_assert_true(strstr(captured_logs,
                           "[tool] result_truncated name=large_output "
                           "original_bytes=60000") != NULL);

  reset_captures(L);
  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_provider_stream_error_event_skips_empty_retry(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_provider_state();
  reset_captures(L);
  int rc = luaL_dostring(
      L,
      "capstan.config = {providers = {deepseek = {"
      "parse_sse_event = function(raw) "
      "return {type = 'provider_error', error = 'invalid_prompt: rejected'} "
      "end"
      "}}}\n");
  munit_assert_int(rc, ==, LUA_OK);

  rc = luaL_dofile(L, "agent/runtime.lua");
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 1);

  call_agent_entry(L);
  munit_assert_int(stream_callback_ref, !=, LUA_NOREF);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushstring(L, "provider-error-event\n\n");
  lua_pushboolean(L, 0);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_rawgeti(L, LUA_REGISTRYINDEX, stream_callback_ref);
  lua_pushnil(L);
  lua_pushboolean(L, 1);
  rc = lua_pcall(L, 2, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int(post_stream_calls, ==, 1);
  munit_assert_true(strstr(captured_logs,
                           "[stream] provider_error=invalid_prompt: rejected") != NULL);
  munit_assert_true(strstr(captured_logs,
                           "[agent] stream failed error=invalid_prompt: rejected") != NULL);
  munit_assert_true(strstr(captured_logs, "requesting finalization") == NULL);
  munit_assert_true(strstr(captured_logs, "[stream] empty_response") == NULL);

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
    {"/session_title_generation_is_silent",
     test_session_title_generation_is_silent, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/failed_response_skips_session_title",
     test_failed_response_skips_session_title, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/model_start_callback_reports_effective_request_context",
     test_model_start_callback_reports_effective_request_context, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/model_observer_failure_stops_run_cleanly",
     test_model_observer_failure_stops_run_cleanly, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/sync_post_stream_failure_finishes_cleanly",
     test_sync_post_stream_failure_finishes_cleanly, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/interactive_options_override_request",
     test_interactive_options_override_request, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/interactive_options_reject_unknown_provider",
     test_interactive_options_reject_unknown_provider, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/interactive_options_reject_unknown_profile",
     test_interactive_options_reject_unknown_profile, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/interactive_cli_profile_can_be_switched",
     test_interactive_cli_profile_can_be_switched, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/profiles_isolation_and_default_replacement",
     test_profiles_isolation_and_default_replacement, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/request_enables_auto_tool_choice", test_request_enables_auto_tool_choice,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/request_applies_reasoning_effort", test_request_applies_reasoning_effort,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/openrouter_keeps_nested_reasoning_effort",
     test_openrouter_keeps_nested_reasoning_effort, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/reasoning_details_preserved_for_tool_continuation",
     test_reasoning_details_preserved_for_tool_continuation, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_merges_reasoning_detail_fragments",
     test_stream_merges_reasoning_detail_fragments, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/reasoning_plaintext_preserved_and_config_can_disable",
     test_reasoning_plaintext_preserved_and_config_can_disable, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/config_applies_reasoning_effort", test_config_applies_reasoning_effort,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/agent_reasoning_effort_accessor", test_agent_reasoning_effort_accessor,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/default_profile_is_implement", test_default_profile_is_implement, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/mcp_initializes_lazily_after_startup",
     test_mcp_initializes_lazily_after_startup, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/mcp_image_result_becomes_multimodal_message",
     test_mcp_image_result_becomes_multimodal_message, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/http_mcp_initializes_without_stdio_spawn",
     test_http_mcp_initializes_without_stdio_spawn, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/http_mcp_initialize_error_marks_server_failed",
     test_http_mcp_initialize_error_marks_server_failed, NULL, NULL,
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
    {"/builtin_profiles_are_plan_and_implement",
     test_builtin_profiles_are_plan_and_implement, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_tool_enabled_by_default", test_subagents_tool_enabled_by_default,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_tool_disabled_by_capability",
     test_subagents_tool_disabled_by_capability, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_tool_returns_structured_results",
     test_subagents_tool_returns_structured_results, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_reject_unavailable_tools_before_launch",
     test_subagents_reject_unavailable_tools_before_launch, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/plan_subagents_inherit_profile_and_readonly_tools",
     test_plan_subagents_inherit_profile_and_readonly_tools, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_respect_explicit_small_max_turns",
     test_subagents_respect_explicit_small_max_turns, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_cap_model_requested_limits",
     test_subagents_cap_model_requested_limits, NULL, NULL,
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
    {"/subagents_discard_failed_output_and_raw_error_body",
     test_subagents_discard_failed_output_and_raw_error_body, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagents_bound_successful_output_with_valid_utf8",
     test_subagents_bound_successful_output_with_valid_utf8, NULL, NULL,
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
    {"/provider_env_context_limit_applies_to_active_provider",
     test_provider_env_context_limit_applies_to_active_provider, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_set_for_persists_active_provider",
     test_provider_models_set_for_persists_active_provider, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_models_set_publishes_effective_effort",
     test_provider_models_set_publishes_effective_effort, NULL, NULL,
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
    {"/compact_whitespace_result_preserves_history",
     test_compact_whitespace_result_preserves_history, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/compact_provider_error_preserves_history_and_finishes",
     test_compact_provider_error_preserves_history_and_finishes, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/auto_compact_estimates_pending_request_and_can_disable",
     test_auto_compact_estimates_pending_request_and_can_disable, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/token_estimate_is_conservative_for_utf8",
     test_token_estimate_is_conservative_for_utf8, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/compact_skips_weak_model_with_smaller_context",
     test_compact_skips_weak_model_with_smaller_context, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/auto_compact_skips_weak_model_with_unknown_context",
     test_auto_compact_skips_weak_model_with_unknown_context, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_schema_requires_known_argument_without_composition",
     test_file_read_schema_requires_known_argument_without_composition, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
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
    {"/plugin_tools_collection_and_permission_false",
     test_plugin_tools_collection_and_permission_false, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_permission_target_uses_path",
     test_wiki_ingest_permission_target_uses_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_yes_persists_file_read_permission",
     test_wiki_ingest_yes_persists_file_read_permission, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/logs_text_when_model_does_not_call_tool",
     test_logs_text_when_model_does_not_call_tool, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/logs_empty_stream_completion", test_logs_empty_stream_completion, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/repeated_empty_terminal_response_fails_visibly",
     test_repeated_empty_terminal_response_fails_visibly, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_error_finishes_with_error",
     test_stream_error_finishes_with_error, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/stream_error_empty_body_includes_request_id",
     test_stream_error_empty_body_includes_request_id, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/transient_stream_error_retries_before_output",
     test_transient_stream_error_retries_before_output, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/vcs_unborn_diff_is_complete_and_disables_git_extensions",
     test_vcs_unborn_diff_is_complete_and_disables_git_extensions, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/vcs_permission_target_uses_workspace_root",
     test_vcs_permission_target_uses_workspace_root, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_permission_target_uses_path",
     test_file_read_permission_target_uses_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_inside_wiki_routes_without_permission",
     test_file_read_inside_wiki_routes_without_permission, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_observer_failure_stops_before_execution",
     test_tool_observer_failure_stops_before_execution, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/permission_error_finishes_tool_event",
     test_permission_error_finishes_tool_event, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/execution_error_finishes_tool_event",
     test_execution_error_finishes_tool_event, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_observer_preserves_raw_and_effective_arguments",
     test_tool_observer_preserves_raw_and_effective_arguments, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/no_wiki_hides_tools_and_disables_configured_path",
     test_no_wiki_hides_tools_and_disables_configured_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/sensitive_file_read_forces_prompt",
     test_sensitive_file_read_forces_prompt, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/explicit_allow_reads_sensitive_file_without_prompt",
     test_explicit_allow_reads_sensitive_file_without_prompt, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/chunked_tool_call_arguments_continue_with_tool_result",
     test_chunked_tool_call_arguments_continue_with_tool_result, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/stream_tool_delta_logs_only_at_debug",
     test_stream_tool_delta_logs_only_at_debug, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/streamed_file_edit_tool_edits_file",
     test_streamed_file_edit_tool_edits_file, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/unvalidated_multi_file_write_starts_completion_review",
     test_unvalidated_multi_file_write_starts_completion_review, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/write_after_validation_restores_completion_review",
     test_write_after_validation_restores_completion_review, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_tool_uses_tool_args_path",
     test_file_read_tool_uses_tool_args_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_image_becomes_multimodal_message",
     test_file_read_image_becomes_multimodal_message, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/file_read_non_image_binary_is_not_inserted_as_text",
     test_file_read_non_image_binary_is_not_inserted_as_text, NULL, NULL,
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
    {"/tool_activity_wraps_top_level_execution",
     test_tool_activity_wraps_top_level_execution, NULL, NULL,
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
    {"/tool_guard_soft_skips_redundant_generated_output_checks",
     test_tool_guard_soft_skips_redundant_generated_output_checks, NULL, NULL,
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
    {"/tool_guard_finishes_event_before_stopping_run",
     test_tool_guard_finishes_event_before_stopping_run, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_default_duration_is_2700_seconds",
     test_tool_guard_default_duration_is_2700_seconds, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_pauses_during_permission_prompt",
     test_tool_guard_pauses_during_permission_prompt, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_guard_stops_max_turns_before_continuation_request",
     test_tool_guard_stops_max_turns_before_continuation_request, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_always_allow_is_session_scoped",
     test_shell_always_allow_is_session_scoped, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/yolo_persists_across_interactive_sessions",
     test_yolo_persists_across_interactive_sessions, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/yolo_does_not_mutate_external_permission_scope",
     test_yolo_does_not_mutate_external_permission_scope, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/yolo_allows_sensitive_path_without_prompt",
     test_yolo_allows_sensitive_path_without_prompt, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/yolo_preserves_explicit_deny", test_yolo_preserves_explicit_deny,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
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
    {"/workdir_only_full_control_allows_workspace_shell",
     test_workdir_only_full_control_allows_workspace_shell, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/workdir_only_full_control_denies_outside_shell_path",
     test_workdir_only_full_control_denies_outside_shell_path, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/workdir_only_tracks_nested_shell_cd",
     test_workdir_only_tracks_nested_shell_cd, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/workdir_only_checks_leading_redirection_paths",
     test_workdir_only_checks_leading_redirection_paths, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/failed_shell_validation_is_not_successful",
     test_failed_shell_validation_is_not_successful, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/request_includes_workdir_and_workspace_root",
     test_request_includes_workdir_and_workspace_root, NULL, NULL,
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
    {"/isolated_runtime_ignores_config_agent_and_hooks",
     test_isolated_runtime_ignores_config_agent_and_hooks, NULL, NULL,
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
    {"/stream_preserves_split_crlf_event",
     test_stream_preserves_split_crlf_event, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_parse_sse_event_override",
     test_provider_parse_sse_event_override, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/json_array_preserves_empty_array_encoding",
     test_json_array_preserves_empty_array_encoding, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/runtime_sanitizes_invalid_utf8_before_request",
     test_runtime_sanitizes_invalid_utf8_before_request, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/tool_output_is_bounded_before_continuation",
     test_tool_output_is_bounded_before_continuation, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_stream_error_event_skips_empty_retry",
     test_provider_stream_error_event_skips_empty_retry, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/hook_error_logs_and_keeps_request",
     test_hook_error_logs_and_keeps_request, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite provider_tools_suite = {"/provider_tools", tests, NULL, 1,
                                   MUNIT_SUITE_OPTION_NONE};
