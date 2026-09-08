/* Standalone bridge driver; each invocation owns a fresh native singleton. */
#include "telemetry.h"
#include "log.h"
#include "redact.h"
#include <stdlib.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Never print arbitrary diagnostic strings (including collector responses). */
int log_event_level(const char *level, const char *category, const char *message) {
  (void)level; (void)category; (void)message;
  fputs("unexpected synchronous diagnostic\n", stderr);
  abort();
}
static int redact(lua_State *L) {
  char *value = redact_secrets_alloc(luaL_checkstring(L, 1));
  if (!value) return luaL_error(L, "redaction allocation failed");
  lua_pushstring(L, value); free(value); return 1;
}
static void setup(lua_State *L) {
  luaL_openlibs(L);
  lua_pushcfunction(L, redact); lua_setglobal(L, "fixture_redact");
}
const char *log_session_id(void) { return "native-session"; }
int log_event_correlated(const char *level, const char *category,
                         const char *message, const char *session_id,
                         const char *trace_id, const char *span_id) {
  (void)level; (void)category; (void)message; (void)session_id;
  (void)trace_id; (void)span_id;
  return 1;
}
static int script(lua_State *L, const char *s) {
  if (luaL_dostring(L, s) == LUA_OK) return 1;
  /* Test assertions contain no user data; still avoid printing Lua values. */
  fputs("native driver Lua assertion failed\n", stderr);
  lua_pop(L, 1); return 0;
}
int main(int argc, char **argv) {
  if (argc != 3) { fputs("usage: test_telemetry_native MODE BASE_ENDPOINT\n", stderr); return 2; }
  lua_State *L = luaL_newstate();
  if (!L) return 2;
  setup(L);
  if (!strcmp(argv[1], "startup") || !strcmp(argv[1], "startup-disabled")) {
    telemetry_startup();
    telemetry_startup(); /* Repeated startup retains a single event. */
  }
  lua_pushstring(L, argv[1]); lua_setglobal(L, "mode");
  lua_pushstring(L, argv[2]); lua_setglobal(L, "base");
  int ok = script(L,
    "capstan={config={observability={enabled=true,endpoint=base,"
    "service_name='native-test',service_version='test-1',"
    "headers={['X-Test']='config'},resource_attributes={suite='native'}}}} "
    "package.preload['agent.redact']=function() return {text=function(s) "
    "s=fixture_redact(s); if mode=='redaction' then "
    "s=s:gsub('CUSTOM_PRIVATE','[CUSTOM]') end; return s end} end "
    "local c=capstan.config.observability "
    "if mode=='redaction' then c.resource_attributes.suite='CUSTOM_PRIVATE'; "
    "c.service_name='CUSTOM_PRIVATE' end "
    "if mode=='disabled' or mode=='startup-disabled' then c.enabled=false end "
    "if mode=='specific' or mode=='precedence' then "
    "c.traces_endpoint=base..'/custom/traces'; c.logs_endpoint=base..'/custom/logs'; "
    "c.traces_headers={['X-Test']='trace-config'}; c.logs_headers={['X-Test']='log-config'} end "
    "if mode=='overflow' or mode=='single' or mode=='retry' then c.logs_exporter='none' end");
  telemetry_init(L, !strcmp(argv[1], "isolated"));
  /* Re-registration must not reset the singleton or change its configuration. */
  if (ok) ok = script(L, "capstan.config.observability.endpoint='http://127.0.0.1:1/never'");
  telemetry_init(L, 0);
  if (ok) ok = script(L,
    "local t=capstan.telemetry "
    "local p=t.start('agent.run',nil,{operation='run',depth=0,"
    "prompt='FORBIDDEN_RAW',messages='FORBIDDEN_RAW',headers='FORBIDDEN_RAW',"
    "url='FORBIDDEN_RAW',arguments='FORBIDDEN_RAW',output='FORBIDDEN_RAW',"
    "provider='api_key=FAKE_TEST_VALUE',model='fixture-model',count=-1,attempt=0/0}) "
    "if not p then print('disabled') return end "
    "print('enabled') "
    "assert(p.session_id=='native-session') "
    "if mode=='context' then "
    "p.session_id='mutated-session'; "
    "local child=assert(t.start('model',p)); "
    "assert(child.session_id=='native-session'); "
    "assert(t.end_span(child,true,false)); assert(t.end_span(p,true,false)); "
    "p.session_id='completed-session'; "
    "local title=assert(t.start('title',p)); "
    "assert(title.session_id=='completed-session'); "
    "assert(title.trace_id==p.trace_id); assert(t.end_span(title,true,false)) "
    "elseif mode=='redaction' then "
    "local s=assert(t.start('tool',p,{model='CUSTOM_PRIVATE'})); "
    "assert(t.end_span(s,true,false,{profile='CUSTOM_PRIVATE'})); "
    "assert(t.end_span(p,true,false)) "
    "elseif mode=='alternation' then "
    "assert(t.end_span(p,true,false)); for i=2,300 do "
    "local s=assert(t.start('tool',nil,{count=i})); assert(t.end_span(s,true,false)) end "
    "elseif mode=='states' then shared=p "
    "elseif mode=='overflow' then "
    "local spans={p}; for i=2,1024 do spans[i]=assert(t.start('tool',p)) end "
    "assert(t.start('tool',p)==nil) "
    "for _,s in ipairs(spans) do assert(t.end_span(s,true,false)) end "
    "for i=1,1100 do local s=assert(t.start('model')); assert(t.end_span(s,true,false)) end "
    "elseif mode=='single' or mode=='retry' then assert(t.end_span(p,true,false)) "
    "else "
    "local m=assert(t.start('agent.model',p,{input_tokens=12,profile='fixture'})) "
    "local tool=assert(t.start('agent.tool',m,{tool='shell'})) "
    "assert(t.end_span(tool,false,true,{output='FORBIDDEN_RAW',duration_ms=2.5})) "
    "assert(not t.end_span(tool,true,false)) "
    "assert(t.end_span(m,false,false,{output_tokens=3})) "
    "assert(t.end_span(p,true,false)) "
    "assert(not t.end_span(p,true,false)) "
    "end");
  if (ok && !strcmp(argv[1], "states")) {
    lua_State *other = luaL_newstate();
    if (!other) { lua_close(L); return 2; }
    setup(other);
    ok = script(other, "package.preload['agent.redact']=function() return {text=fixture_redact} end");
    telemetry_init(other, 0);
    lua_getglobal(L, "shared");
    lua_newtable(other);
    const char *keys[] = {"trace_id", "span_id"};
    for (int i=0;i<2;i++) {
      lua_getfield(L, -1, keys[i]);
      lua_pushstring(other, lua_tostring(L, -1)); lua_setfield(other, -2, keys[i]);
      lua_pop(L, 1);
    }
    lua_pop(L, 1); lua_setglobal(other, "foreign");
    if (ok) ok = script(other,
      "saved=capstan.telemetry; assert(saved.diagnostics().enabled); "
      "assert(not saved.end_span(foreign,true,false)); "
      "local s=assert(saved.start('model')); assert(saved.end_span(s,true,false))");
    telemetry_init(other, 1);
    telemetry_init(other, 0);
    if (ok) ok = script(other,
      "for _,t in ipairs({saved,capstan.telemetry}) do "
      "assert(t.start('tool',foreign)==nil); assert(not t.end_span(foreign,true,false)); "
      "assert(t.diagnostics()==nil) end");
    lua_close(other);
    if (ok) ok = script(L,
      "assert(capstan.telemetry.diagnostics().enabled); "
      "assert(capstan.telemetry.end_span(shared,true,false))");
  }
  if (!strcmp(argv[1], "retry")) {
    struct timespec start, now, pause = {0, 10000000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
      telemetry_poll(); nanosleep(&pause, NULL);
      clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < 8);
  } else if (!strcmp(argv[1], "overflow")) telemetry_poll();
  telemetry_cleanup();
  if (ok) ok = script(L,
    "local d=capstan.telemetry.diagnostics(); "
    "if mode=='isolated' then assert(d==nil) return end "
    "assert(type(d)=='table' and d.enabled==false and d.queued==0); "
    "for _,k in ipairs({'dropped','rejected','failed','malformed','configuration_errors'}) do "
    "assert(math.type(d[k])=='integer' and d[k]>=0) end "
    "if d.dropped+d.rejected+d.failed+d.malformed>0 then "
    "print(string.format('losses %d %d %d %d',d.dropped,d.rejected,d.failed,d.malformed)) end");
  lua_close(L);
  return ok ? 0 : 1;
}
