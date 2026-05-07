#include "agent.h"
#include "http.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  http_init(L);
  agent_init(L);

  if (luaL_dostring(L,
      "chunks = {}; done = false\n"
      "async_id = http.post_stream("
      "  'https://httpbin.org/post',"
      "  '{\"test\":\"stream\"}',"
      "  {['Content-Type']='application/json'},"
      "  function(raw, is_done)"
      "    if is_done then done = true"
      "    else"
      "      table.insert(chunks, raw)"
      "      agent.append(raw)"
      "    end"
      "  end"
      ")\n"
      "print('async_id=' .. async_id)\n") != LUA_OK) {
    fprintf(stderr, "init error: %s\n", lua_tostring(L, -1));
    return 1;
  }

  printf("Polling...\n");
  for (int i = 0; i < 500; i++) {
    lua_getglobal(L, "done");
    int done = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (done) break;

    http_poll(L);
    { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }
  }

  lua_getglobal(L, "done");
  int done = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getglobal(L, "chunks");
  int n = (int)luaL_len(L, -1);
  lua_pop(L, 1);

  Messages *msgs = get_messages();

  printf("done=%d  chunks=%d\n", done, n);
  for (int i = 0; i < msgs->count; i++) {
    Message *m = msgs->items[i];
    printf("  msg[%d] role=%s text[%zu]='%s'\n",
           i, m->role == MSG_USER ? "USER" : "AGENT",
           strlen(m->text ? m->text : ""),
           m->text ? m->text : "(null)");
  }

  lua_close(L);
  http_cleanup();
  return done ? 0 : 1;
}
