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
#include <curl/curl.h>

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
static int switch_context(lua_State *L) {
  telemetry_set_context(luaL_optstring(L,1,"tui"), luaL_optstring(L,2,""),
                        luaL_optstring(L,3,""));
  return 0;
}
static void setup(lua_State *L) {
  luaL_openlibs(L);
  lua_pushcfunction(L, switch_context); lua_setglobal(L, "switch_context");
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
static int exporter_active;
static unsigned local_events;
static const LogAttribute *find_attr(const LogAttribute *attrs, size_t count,
                                      const char *key) {
  for (size_t i=0;i<count;i++) if (!strcmp(attrs[i].key,key)) return &attrs[i];
  return NULL;
}
#define CHECK(c) do { if (!(c)) { fputs("local adapter assertion failed\n",stderr); abort(); } } while (0)
int log_event_structured(const char *level, const char *category,
                         const char *message, const char *session_id,
                         const char *trace_id, const char *span_id,
                         const LogAttribute *attrs, size_t count) {
  CHECK(!exporter_active);
  CHECK(!strcmp(category,"telemetry"));
  (void)level; (void)session_id;
  if (!strcmp(message,"exporter.diagnostics")) {
    const char *keys[]={"dropped","rejected","failed","malformed",
                        "configuration_errors","queued","enabled"};
    CHECK((count==7 || count==8) && !trace_id && !span_id);
    if (count==8) {
      CHECK(!strcmp(attrs[7].key,"error.category"));
      CHECK(!strncmp(attrs[7].text,"exporter_",9));
    }
    for (size_t i=0;i<7;i++) {
      CHECK(!strcmp(attrs[i].key,keys[i]));
      CHECK(attrs[i].type==(i==6 ? 2 : 1) && attrs[i].number>=0);
    }
    return 1;
  }
  CHECK(trace_id && strlen(trace_id)==32 && span_id && strlen(span_id)==16);
  CHECK(find_attr(attrs,count,"span.name"));
  const LogAttribute *run=find_attr(attrs,count,"run.id");
  CHECK(run && run->type==0 && strlen(run->text)==16);
  for (size_t i=0;i<count;i++) {
    CHECK(!strstr(attrs[i].text,"FORBIDDEN_RAW"));
    CHECK(!strstr(attrs[i].text,"FAKE_TEST_VALUE"));
    CHECK(!strstr(attrs[i].text,"CUSTOM_PRIVATE"));
  }
  if (!local_events++) {
    CHECK(!strcmp(message,"span.started"));
    CHECK(find_attr(attrs,count,"operation") &&
          !strcmp(find_attr(attrs,count,"operation")->text,"run"));
    CHECK(find_attr(attrs,count,"depth") &&
          find_attr(attrs,count,"depth")->number==0);
    CHECK(!find_attr(attrs,count,"count") && !find_attr(attrs,count,"attempt"));
    CHECK(!strcmp(find_attr(attrs,count,"model")->text,"fixture-model"));
    CHECK(!strcmp(find_attr(attrs,count,"session.id")->text,"native-session"));
  }
  if (!strcmp(message,"span.finished")) {
    CHECK(find_attr(attrs,count,"cancelled")->type==2);
    CHECK(find_attr(attrs,count,"outcome")->type==0);
    const LogAttribute *category_attr=find_attr(attrs,count,"error.category");
    if (strcmp(find_attr(attrs,count,"outcome")->text,"error")) CHECK(!category_attr);
    if (category_attr) CHECK(!strcmp(category_attr->text,"transport"));
  }
  return 0; /* Local write failure must not prevent native completion/export. */
}
static int script(lua_State *L, const char *s) {
  if (luaL_dostring(L, s) == LUA_OK) return 1;
  /* Test assertions contain no user data; still avoid printing Lua values. */
  fputs("native driver Lua assertion failed\n", stderr);
  lua_pop(L, 1); return 0;
}
static int test_exporter_result_category(void) {
  static const struct { int code; const char *category; } cases[] = {
    {CURLE_COULDNT_RESOLVE_HOST,"exporter_dns"},
    {CURLE_COULDNT_RESOLVE_PROXY,"exporter_dns"},
    {CURLE_COULDNT_CONNECT,"exporter_connect"},
    {CURLE_SSL_CONNECT_ERROR,"exporter_tls"},
    {CURLE_PEER_FAILED_VERIFICATION,"exporter_tls"},
    {CURLE_SSL_CERTPROBLEM,"exporter_tls"},
    {CURLE_SSL_CIPHER,"exporter_tls"},
    {CURLE_SSL_CACERT_BADFILE,"exporter_tls"},
    {CURLE_SSL_CRL_BADFILE,"exporter_tls"},
    {CURLE_SSL_ISSUER_ERROR,"exporter_tls"},
    {CURLE_USE_SSL_FAILED,"exporter_tls"},
    {CURLE_SSL_PINNEDPUBKEYNOTMATCH,"exporter_tls"},
    {CURLE_SSL_INVALIDCERTSTATUS,"exporter_tls"},
    {CURLE_LOGIN_DENIED,"exporter_auth"},
    {CURLE_OPERATION_TIMEDOUT,"exporter_transport"},
    {CURLE_SEND_ERROR,"exporter_transport"},
    {CURLE_RECV_ERROR,"exporter_transport"},
    {CURLE_GOT_NOTHING,"exporter_transport"},
    {CURLE_WRITE_ERROR,"exporter_transport"},
    {-1,"exporter_transport"}, {9999,"exporter_transport"}
  };
  static const long statuses[] = {0,200,201,204,307,400,401,403,407,429,500,502,503,504};
  for (size_t j=0;j<sizeof(statuses)/sizeof(statuses[0]);j++) {
    long status=statuses[j];
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
      const char *actual=telemetry_exporter_result_category(cases[i].code,status);
      if (!actual || strcmp(actual,cases[i].category)) return 1;
    }
    const char *actual=telemetry_exporter_result_category(CURLE_OK,status);
    if (status==200) { if (actual) return 1; }
    else if (!actual || strcmp(actual,
        status==401 || status==403 || status==407 ? "exporter_auth" : "exporter_http")) return 1;
  }
  puts("classification ok");
  return 0;
}
int main(int argc, char **argv) {
  if (argc==3 && !strcmp(argv[1],"classification"))
    return test_exporter_result_category();
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
    "if mode=='details' then c.include_tool_details=true end "
    "if mode=='details-invalid' then c.include_tool_details='true' end "
    "if mode=='context' then c.include_session_name=true; "
    "switch_context('tui','native-session','api_key=FAKE_TEST_VALUE') end "
    "if mode=='redaction' then c.resource_attributes.suite='CUSTOM_PRIVATE'; "
    "c.service_name='CUSTOM_PRIVATE' end "
    "if mode=='disabled' or mode=='startup-disabled' or mode=='offline-silent' then c.enabled=false end "
    "if mode=='offline-silent' or mode=='network-only' then c.file_exporter=false end "
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
    "if mode=='isolated' then assert(p==nil); print('disabled'); return end "
    "assert(type(p)=='table' and #p.trace_id==32 and #p.span_id==16) "
    "print(t.diagnostics().enabled and 'enabled' or 'disabled') "
    "assert(p.session_id=='native-session') "
    "if mode=='phases' then "
    "local keys={'namelookup_elapsed_ms','connect_elapsed_ms','appconnect_elapsed_ms',"
    "'pretransfer_elapsed_ms','starttransfer_elapsed_ms','total_ms','dns_ms',"
    "'tcp_connect_ms','tls_handshake_ms','request_setup_ms',"
    "'upload_and_server_wait_ms','download_ms'} "
    "local a={} for i,k in ipairs(keys) do a['transport.'..k]=i/2 end "
    "a['transport.url']='FORBIDDEN_RAW'; a['transport.upload_bytes']=123; "
    "a['transport.download_bytes']=456; assert(t.end_span(p,true,false,a)) "
    "elseif mode=='roots' then "
    "local first_run=p.run_id; local first_trace=p.trace_id; "
    "switch_context('cli','second-session','Second'); "
    "local next_root=assert(t.start('run')); "
    "assert(next_root.run_id~=first_run); assert(next_root.trace_id~=first_trace); "
    "assert(next_root.session_id=='second-session'); "
    "p.run_id='forged'; p.trace_id=string.rep('1',32); p.span_id=string.rep('2',16); "
    "assert(t.end_span(p,true,false)); "
    "switch_context('acp',nil,nil); "
    "local child=assert(t.start('title',p)); "
    "assert(child.run_id==first_run); assert(child.trace_id==first_trace); "
    "assert(child.session_id=='native-session'); assert(t.end_span(child,true,false)); "
    "local ephemeral=assert(t.start('run')); assert(ephemeral.session_id==''); "
    "assert(ephemeral.run_id~=next_root.run_id); "
    "assert(t.end_span(ephemeral,true,false)); assert(t.end_span(next_root,true,false)) "
    "elseif mode=='context' then "
    "p.session_id='mutated-session'; "
    "local child=assert(t.start('model',p)); "
    "assert(child.session_id=='native-session'); "
    "assert(t.end_span(child,true,false)); assert(t.end_span(p,true,false)); "
    "switch_context('acp','switched-session','Renamed session'); "
    "p.session_id='completed-session'; "
    "local title=assert(t.start('title',p)); "
    "assert(title.session_id=='native-session'); "
    "assert(title.run_id==p.run_id); "
    "assert(title.trace_id==p.trace_id); assert(t.end_span(title,true,false)) "
    "elseif mode=='details' or mode=='details-off' or mode=='details-invalid' then "
    "local s=assert(t.start('subagent',p,{subagent_index=2,subagent_id='api_key=FAKE_TEST_VALUE',attempt=1,"
    "['shell.command']='echo api_key=FAKE_TEST_VALUE',['tool.target']='fixture.lua',"
    "env='FORBIDDEN_RAW',stdin='FORBIDDEN_RAW',arguments='FORBIDDEN_RAW'})); "
    "assert(t.end_span(s,true,false)); "
    "local bounded=assert(t.start('tool',p)); "
    "assert(t.end_span(bounded,true,false,{['shell.command']=string.rep('x',129),"
    "['tool.target']='api_key=FAKE_TEST_VALUE'})); assert(t.end_span(p,true,false)) "
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
    "elseif mode=='categories' then "
    "for _,v in ipairs({'transport','FORBIDDEN_RAW','transport\\0suffix',42}) do "
    "local s=assert(t.start('tool',p,{['error.category']='transport'})); "
    "assert(t.end_span(s,false,false,{['error.category']=v})) end "
    "local s=assert(t.start('tool',p)); "
    "assert(t.end_span(s,false,true,{['error.category']='transport'})); "
    "assert(t.end_span(p,true,false,{['error.category']='transport'})) "
    "elseif mode=='single' or mode=='retry' then assert(t.end_span(p,true,false)) "
    "else "
    "local m=assert(t.start('agent.model',p,{input_tokens=12,profile='fixture'})) "
    "local tool=assert(t.start('agent.tool',m,{tool='shell'})) "
    "assert(t.end_span(tool,false,true,{output='FORBIDDEN_RAW',duration_ms=2.5,['error.category']='transport'})) "
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
  if (!strcmp(argv[1],"offline-silent") || !strcmp(argv[1],"network-only") ||
      !strcmp(argv[1],"isolated")) CHECK(local_events==0);
  if (!strcmp(argv[1],"disabled")) CHECK(local_events==6);
  exporter_active=1;
  if (!strcmp(argv[1], "retry")) {
    struct timespec start, now, pause = {0, 10000000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
      telemetry_poll(); nanosleep(&pause, NULL);
      clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < 8);
  } else if (!strcmp(argv[1], "overflow")) telemetry_poll();
  telemetry_cleanup();
  exporter_active=0;
  if (ok) ok = script(L,
    "assert(capstan.telemetry.start('run')==nil); "
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
