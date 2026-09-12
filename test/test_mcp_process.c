/* Standalone integration test: real Lua MCP binding, fake UI pump. */
#include "process_manager.h"
#include <assert.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void mcp_init(lua_State *L);
void mcp_cleanup(void);
static int pumps;
static char stop_on_pump[PROCESS_ID_SIZE];
void tui_pump_blocking(void) {
  pumps++;
  process_manager_poll();
  if (stop_on_pump[0]) {
    assert(process_manager_stop(stop_on_pump));
    stop_on_pump[0] = 0;
  }
}
static void run(lua_State *L, const char *script) {
  if (luaL_dostring(L, script) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    mcp_cleanup();
    process_manager_shutdown();
    exit(1);
  }
}
int main(void) {
  signal(SIGPIPE, SIG_IGN); /* Same policy as the application entry point. */
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  mcp_init(L);
  process_manager_set_owner("active-session");
  run(L, "h = mcp.spawn('/bin/cat', {'-u'}, {}); assert(mcp.alive(h)); "
         "assert(mcp.send(h, '{\"id\":1}')); "
         "assert(mcp.recv(h, 2000) == '{\"id\":1}')");
  ProcessSnapshot global;
  assert(process_manager_at(process_manager_count() - 1, &global));
  assert(!strcmp(global.kind, "mcp"));
  assert(!strcmp(global.label, "/bin/cat -u"));
  assert(!strcmp(global.owner, "runtime"));
  assert(!strcmp(process_manager_owner(), "active-session"));
  char *out = process_manager_raw_output(global.id, 0);
  assert(out && !out[0]); /* NDJSON is protocol-owned, not captured output. */
  free(out);
  assert(process_manager_stop(global.id));
  run(L, "assert(not mcp.alive(h)); local ok, err = mcp.send(h, 'x'); "
         "assert(not ok and err:find('reconnect')); assert(not mcp.recv(h, 10))");
  size_t count = process_manager_count();
  run(L, "for i=1,5 do assert(not mcp.alive(h)) end");
  assert(process_manager_count() == count); /* Never auto-restart. */

  run(L, "s = mcp.spawn('/bin/cat', {}, {}, 'acp-session-42'); assert(mcp.alive(s)); "
         "assert(mcp.send(s, 'scoped')); assert(mcp.recv(s, 2000) == 'scoped')");
  ProcessSnapshot scoped;
  assert(process_manager_at(process_manager_count() - 1, &scoped));
  assert(!strcmp(scoped.owner, "acp-session-42"));
  assert(!strcmp(process_manager_owner(), "active-session"));
  snprintf(stop_on_pump, sizeof(stop_on_pump), "%s", scoped.id);
  run(L, "local line, err = mcp.recv(s, 2000); assert(not line and err == 'process exited'); "
         "assert(not mcp.alive(s)); assert(not mcp.recv_nowait(s))");
  assert(pumps > 0);
  run(L, "assert(not pcall(mcp.spawn, './missing-mcp-test-executable', {}, {}, 'failed-scope'))");
  assert(!strcmp(process_manager_owner(), "active-session"));
  mcp_cleanup();
  process_manager_shutdown();
  lua_close(L);
  puts("MCP process integration passed");
  return 0;
}
