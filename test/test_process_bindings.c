#include "permit.h"
#include "process_manager.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

lua_State *L;
const char *session_manager_active_id(void) { return "manual-session"; }
int log_event(const char *category, const char *text) { (void)category; (void)text; return 1; }
const char *tui_permit_prompt(const char *tool, const char *target) {
  (void)tool; (void)target; return "deny";
}
void tui_pump_blocking(void) { process_manager_poll(); }
static void run(const char *script) {
  if (luaL_dostring(L, script) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    process_manager_shutdown(); exit(1);
  }
}
int main(void) {
  L = luaL_newstate(); assert(L); luaL_openlibs(L); tools_init(L);
  run("tools.process_scope('session-a'); a = tools.shell('printf ready; sleep 30', 0, true); "
      "assert(a.started and a.id and a.pid and not a.exit); "
      "tools.process_scope('session-b'); assert(#tools.processes('list') == 0); "
      "assert(not pcall(tools.processes, 'get', a.id)); "
      "assert(not pcall(tools.process_stop, a.id)); "
      "assert(not pcall(tools.process_stop, tostring(a.pid))); "
      "b = tools.shell('sleep 30', 0, true); assert(tools.process_close_owner('session-a')); "
      "assert(tools.processes('get', b.id).running); tools.process_scope('session-a'); "
      "assert(tools.processes('get', a.id).stopping); "
      "assert(not tools.processes('get', a.id).running)");
  long long end = process_manager_now_ms() + 4000;
  ProcessSnapshot s;
  do {
    process_manager_poll();
    assert(process_manager_at(0, &s));
    assert(process_manager_now_ms() < end);
    struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
  } while (s.running);
  run("assert(#tools.process_events('session-b') == 0); "
      "local events = tools.process_events('session-a'); assert(#events == 1 and events[1].id == a.id); "
      "assert(#tools.process_events('session-a') == 0); "
      "tools.process_scope('session-b'); assert(tools.process_stop(b.id).stopping); "
      "tools.process_scope(nil); local c = tools.shell('true', 2); assert(c.exit == 0)");
  assert(process_manager_at(process_manager_count() - 1, &s));
  assert(!strcmp(s.owner, "manual-session"));
  process_manager_shutdown(); lua_close(L);
  puts("process bindings: ownership, opaque IDs, completion delivery, owner cleanup passed");
  return 0;
}
