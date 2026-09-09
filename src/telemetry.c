#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "telemetry.h"
#include "otlp_wire.h"
#include "log.h"
#include "session.h"
#include <lauxlib.h>
#include "redact.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#ifndef APP_VERSION_VALUE
#define APP_VERSION_VALUE "local"
#endif
#define T_RECORDS 1024
#define T_BYTES (4u * 1024u * 1024u)
#define T_PAYLOAD (256u * 1024u)
#define T_ATTR 8192
#define T_RESOURCE 16384
#define T_PAIRS 64
#define T_SETTINGS 16384
/* Native aliases only; the pure wire module owns bounds and encoding. */
typedef OtlpWire PB;
#define bytes(...) otlp_wire_bytes(__VA_ARGS__)
#define num(...) otlp_wire_num(__VA_ARGS__)
#define fixed(...) otlp_wire_fixed(__VA_ARGS__)
#define blob(...) otlp_wire_blob(__VA_ARGS__)
#define nested(...) otlp_wire_nested(__VA_ARGS__)
static void safe(lua_State *L, char out[129], const char *s, size_t n);
static void text(lua_State *L, PB *b, unsigned f, const char *s) {
  char out[129]; safe(L,out,s,strlen(s)); blob(b,f,out,strlen(out));
}
static uint64_t clock_ns(clockid_t clock) {
  struct timespec t;
  if (clock_gettime(clock, &t)) return 0;
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static uint64_t mono(void) { return clock_ns(CLOCK_MONOTONIC) / 1000000u; }
static uint64_t startup_time, startup_tick;
void telemetry_startup(void) {
  if (!startup_time) {
    startup_time=clock_ns(CLOCK_REALTIME);
    startup_tick=clock_ns(CLOCK_MONOTONIC);
  }
}
static void field(lua_State *L, int i, const char *key) {
  i = lua_absindex(L, i); lua_pushstring(L, key); lua_rawget(L, i);
}
/* Normalize invalid UTF-8/control bytes without destroying valid Unicode.
 * Reject oversized strings wholesale: truncation before redaction could expose
 * a credential prefix that the canonical redactor no longer recognizes. */
/* Protect module lookup as well as custom redaction. Never fall back to C
 * after Lua exists: that would silently bypass the operator's policy. */
static int redact_text(lua_State *L) {
  lua_getglobal(L,"require"); lua_pushliteral(L,"agent.redact"); lua_call(L,1,1);
  lua_getfield(L,-1,"text"); lua_pushvalue(L,1); lua_call(L,1,1); return 1;
}
static void safe(lua_State *L, char out[129], const char *s, size_t n) {
  char redacted[129];
  if (L) {
    int top=lua_gettop(L);
    lua_pushcfunction(L,redact_text); lua_pushlstring(L,s,n);
    int ok=lua_pcall(L,1,1,0)==LUA_OK && lua_type(L,-1)==LUA_TSTRING;
    size_t len=0; const char *r=ok ? lua_tolstring(L,-1,&len) : NULL;
    if (!ok || len>128 || memchr(r,0,len)) {
      strcpy(out,ok ? "[OVERSIZED]" : "[REDACTION_FAILED]");
      lua_settop(L,top); return;
    }
    memcpy(redacted,r,len); redacted[len]=0; n=len; s=redacted;
    lua_settop(L,top);
  }
  if (n>128) { strcpy(out,"[OVERSIZED]"); return; }
  char tmp[129]; size_t at=0;
  while (at<n) {
    unsigned char c=(unsigned char)s[at]; size_t width=1;
    if (c>=0xc2 && c<=0xdf) width=2;
    else if (c>=0xe0 && c<=0xef) width=3;
    else if (c>=0xf0 && c<=0xf4) width=4;
    int valid=c>=32 && c<127;
    if (width>1 && width<=n-at) {
      valid=1;
      for (size_t j=1;j<width;j++) if (((unsigned char)s[at+j]&0xc0)!=0x80) valid=0;
      unsigned char next=(unsigned char)s[at+1];
      if ((c==0xe0 && next<0xa0) || (c==0xed && next>=0xa0) ||
          (c==0xf0 && next<0x90) || (c==0xf4 && next>=0x90)) valid=0;
      if (c==0xc2 && next<0xa0) valid=0;
    }
    if (!valid) { tmp[at++]='?'; continue; }
    memcpy(tmp+at,s+at,width); at+=width;
  }
  tmp[n]=0;
  if (L) { memcpy(out,tmp,n+1); return; }
  char *r=redact_secrets_alloc(tmp);
  if (!r) strcpy(out,"[REDACTION_FAILED]");
  else if (strlen(r)>128) strcpy(out,"[OVERSIZED]");
  else memcpy(out,r,strlen(r)+1);
  free(r);
}
static void attr(lua_State *L, PB *b, unsigned f, const char *key, const char *s,
                 double value, int type) {
  unsigned char kvbuf[384], avbuf[160];
  PB kv = {kvbuf,0,sizeof(kvbuf),0}, av = {avbuf,0,sizeof(avbuf),0};
  text(L,&kv, 1, key);
  if (type == 0) text(L,&av, 1, s);
  else if (type == 1) { uint64_t bits; memcpy(&bits, &value, 8); fixed(&av, 4, bits); }
  else num(&av, 2, value != 0);
  nested(&kv, 2, &av); nested(b, f, &kv);
}
/* Closed vocabulary, accepted only at failed completion. Never classify text.
 * Both lifecycle exporters consume this same canonical attribute policy. */
static void error_attribute(lua_State *L, PB *b, unsigned f, int ok, int cancelled) {
  if (ok || cancelled || !lua_istable(L,4)) return;
  static const char *const categories[] = {
    "transport", "http", "provider", "protocol", "configuration",
    "request", "guard", "empty_response", "tool_unavailable",
    "invalid_arguments", "permission", "tool", "exception", "observer", NULL
  };
  field(L,4,"error.category");
  if (lua_type(L,-1)==LUA_TSTRING) {
    size_t n; const char *s=lua_tolstring(L,-1,&n);
    for (int i=0;categories[i];i++)
      if (strlen(categories[i])==n && !memcmp(s,categories[i],n)) {
        attr(L,b,f,"error.category",categories[i],0,0); break;
      }
  }
  lua_pop(L,1);
}
static int include_tool_details;
static const char *const string_keys[] = {
  "operation", "provider", "model", "profile", "tool", "purpose", "subagent_id",
  "shell.command", "tool.target", NULL
};
static const char *const number_keys[] = {
  "depth", "subagent_index", "attempt", "count", "duration_ms", "input_tokens", "output_tokens",
  "cached_tokens", "reasoning_tokens", "total_tokens", "http_status", "curl_code",
  "request_bytes", "response_bytes", "turns", "tool_count", "request_count",
  "model_ms", "tool_ms", "permission_wait_ms", "subagent_wait_ms", "unattributed_ms", "overlap_ms",
  "turn", "text_bytes", "reasoning_bytes", "tool_calls", "first_output_ms",
  "first_reasoning_ms", "first_text_ms", "first_tool_ms", "events", "raw_bytes",
  "text_chunks", "reasoning_chunks", "tool_delta_chunks", "usage_chunks",
  "usage.prompt_tokens", "usage.completion_tokens", "usage.total_tokens",
  "transport.http_status", "transport.curl_code", "transport.download_bytes",
  "transport.upload_bytes", "transport.chunk_count", "transport.redirect_count",
  "transport.ttfb_ms", "transport.namelookup_elapsed_ms",
  "transport.connect_elapsed_ms", "transport.appconnect_elapsed_ms",
  "transport.pretransfer_elapsed_ms", "transport.starttransfer_elapsed_ms",
  "transport.total_ms", "transport.dns_ms", "transport.tcp_connect_ms",
  "transport.tls_handshake_ms", "transport.request_setup_ms",
  "transport.upload_and_server_wait_ms", "transport.download_ms", NULL
};
static void attributes(lua_State *L, int index, PB *b, unsigned field_number) {
  if (!lua_istable(L, index)) return;
  for (int i = 0; string_keys[i]; i++) {
    if (!include_tool_details && (!strcmp(string_keys[i],"shell.command") ||
        !strcmp(string_keys[i],"tool.target"))) continue;
    field(L, index, string_keys[i]);
    if (lua_type(L, -1) == LUA_TSTRING) {
      size_t n; const char *s = lua_tolstring(L, -1, &n); char out[129];
      safe(L,out, s, n);
      /* Already redacted value; keys still pass through the same policy. */
      unsigned char kvbuf[384], avbuf[160];
      PB kv={kvbuf,0,sizeof(kvbuf),0}, av={avbuf,0,sizeof(avbuf),0};
      text(L,&kv,1,string_keys[i]); blob(&av,1,out,strlen(out));
      nested(&kv,2,&av); nested(b,field_number,&kv);
    }
    lua_pop(L, 1);
  }
  for (int i = 0; number_keys[i]; i++) {
    field(L, index, number_keys[i]);
    if (lua_type(L, -1) == LUA_TNUMBER) {
      double v = lua_tonumber(L, -1);
      if (isfinite(v) && v >= 0) attr(L,b, field_number, number_keys[i], NULL, v, 1);
    }
    lua_pop(L, 1);
  }
}
typedef struct {
  char session_id[SESSION_ID_SIZE];
  char session_name[129], run_id[33], mode[8];
  unsigned char trace[16], span[8];
} Capture;
static Capture current_capture;
static int capture_configured;
static char captures_key;
void telemetry_set_context(const char *mode, const char *session_id,
                           const char *session_name) {
  capture_configured=1;
  memset(&current_capture,0,sizeof(current_capture));
  if (mode && (!strcmp(mode,"cli") || !strcmp(mode,"tui") || !strcmp(mode,"acp")))
    strcpy(current_capture.mode,mode);
  if (session_id && session_id_valid(session_id))
    snprintf(current_capture.session_id,sizeof(current_capture.session_id),"%s",session_id);
  /* Keep the full name until canonical Lua redaction at root start. */
  if (session_name && *session_name)
    snprintf(current_capture.session_name,sizeof(current_capture.session_name),"%s",
             strlen(session_name)>128 ? "[OVERSIZED]" : session_name);
}
static void captures(lua_State *L) {
  lua_rawgetp(L,LUA_REGISTRYINDEX,&captures_key);
  if (!lua_isnil(L,-1)) return;
  lua_pop(L,1); lua_newtable(L); lua_newtable(L);
  lua_pushliteral(L,"k"); lua_setfield(L,-2,"__mode"); lua_setmetatable(L,-2);
  lua_pushvalue(L,-1); lua_rawsetp(L,LUA_REGISTRYINDEX,&captures_key);
}
static void capture_attributes(lua_State *L, const Capture *c, PB *b, unsigned f) {
  if (*c->session_id) attr(L,b,f,"session.id",c->session_id,0,0);
  if (*c->session_name) attr(L,b,f,"session.name",c->session_name,0,0);
  if (*c->run_id) attr(L,b,f,"run.id",c->run_id,0,0);
  if (*c->mode) attr(L,b,f,"mode",c->mode,0,0);
}
typedef struct {
  Capture capture;
  lua_State *owner; uint64_t context;
  int used; unsigned char trace[16], span[8], parent[8];
  uint64_t start, tick; char name[32]; unsigned char attrs[T_ATTR]; size_t n;
  /* Cached start attributes for LogRecord field 6; the same policy owns both
   * encodings. Never retain a caller-owned Lua table until async completion. */
  unsigned char log_attrs[T_ATTR]; size_t log_n;
  char session_id[SESSION_ID_SIZE]; /* Local correlation only; never OTLP. */
} Span;
typedef struct { unsigned char *p; size_t n; int signal; } Record;
static struct {
  int initialized, enabled, file_enabled, flushing, global_curl, include_session_name;
  CURLM *multi; CURL *easy; struct curl_slist *headers[2];
  int signals[2];
  uint64_t configuration_errors;
  const char *error_category; /* Latest exporter failure, never response text. */
  unsigned char scope[272]; size_t scope_n;
  unsigned char resource[T_RESOURCE]; size_t resource_n;
  char endpoint[2][2048], service[129], version[129];
  Span spans[T_RECORDS]; Record queue[T_RECORDS]; size_t count, queued;
  unsigned char payload[T_PAYLOAD], response[8192]; size_t payload_n, response_n;
  size_t batch; int signal, attempts; uint64_t due, retry_after;
  uint64_t dropped, rejected, failed, malformed;
} t;
typedef struct { int allowed; uint64_t generation; } Context;
static char context_key;
static uint64_t context_generation;
static Context *permission(lua_State *L) {
  return (Context *)lua_touserdata(L,lua_upvalueindex(1));
}
static int permitted(lua_State *L) {
  Context *c=permission(L); return c && c->allowed;
}
static int l_diagnostics(lua_State *L) {
  if (!permitted(L)) { lua_pushnil(L); return 1; }
  lua_createtable(L,0,7);
#define STAT(key,value) do { lua_pushinteger(L,(lua_Integer)(value)); lua_setfield(L,-2,key); } while (0)
  STAT("dropped",t.dropped); STAT("rejected",t.rejected);
  STAT("failed",t.failed); STAT("malformed",t.malformed);
  STAT("configuration_errors",t.configuration_errors); STAT("queued",t.count);
#undef STAT
  lua_pushboolean(L,t.enabled); lua_setfield(L,-2,"enabled");
  if (t.error_category) {
    lua_pushstring(L,t.error_category); lua_setfield(L,-2,"error.category");
  }
  /* Explicit pull only: no disk I/O, Lua callbacks or recursive telemetry
   * inside exporter polling/shutdown. Log failures never alter counters. */
  static int logging;
  if (!logging && t.file_enabled) {
    LogAttribute attrs[] = {
      {"dropped","",(double)t.dropped,1},
      {"rejected","",(double)t.rejected,1},
      {"failed","",(double)t.failed,1},
      {"malformed","",(double)t.malformed,1},
      {"configuration_errors","",(double)t.configuration_errors,1},
      {"queued","",(double)t.count,1},
      {"enabled","",t.enabled,2},
      {"error.category","",0,0}
    };
    if (t.error_category)
      snprintf(attrs[7].text,sizeof(attrs[7].text),"%s",t.error_category);
    logging=1;
    (void)log_event_structured("info","telemetry","exporter.diagnostics",
                               log_session_id(),NULL,NULL,attrs,t.error_category ? 8 : 7);
    logging=0;
  }
  return 1;
}
static int random_id(unsigned char *p, size_t n) {
#if defined(__APPLE__) || defined(__FreeBSD__)
  arc4random_buf(p, n);
#elif defined(__linux__)
  /* Nonblocking entropy: telemetry must never stall agent startup. */
  if (getrandom(p, n, GRND_NONBLOCK) != (ssize_t)n) return 0;
#else
  (void)p; (void)n; return 0;
#endif
  unsigned char any = 0; for (size_t i = 0; i < n; i++) any |= p[i];
  return any != 0;
}
static void hex(char *s, const unsigned char *p, size_t n) {
  const char *digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { s[2*i] = digits[p[i] >> 4]; s[2*i+1] = digits[p[i]&15]; }
  s[n*2] = 0;
}
static int id(lua_State *L, int index, const char *key, unsigned char *p, size_t n) {
  if (!lua_istable(L, index)) return 0;
  field(L, index, key); size_t len = 0;
  const char *s = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : NULL;
  int ok = s && len == 2*n, any = 0;
  for (size_t i = 0; ok && i < n; i++) {
    int v = 0;
    for (int j = 0; j < 2; j++) {
      char c = s[2*i+j]; int x = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1;
      if (x < 0) { ok = 0; break; } v = 16*v+x;
    }
    p[i] = (unsigned char)v; any |= v;
  }
  lua_pop(L, 1); return ok && any;
}
static void enqueue(PB *b, int signal) {
  if (!t.enabled || !t.signals[signal]) return;
  if (b->bad || b->n > T_PAYLOAD-T_RESOURCE-256 || t.count == T_RECORDS || b->n > T_BYTES-t.queued) { t.dropped++; return; }
  unsigned char *p = malloc(b->n);
  if (!p) { t.dropped++; return; }
  memcpy(p,b->p,b->n); t.queue[t.count++] = (Record){p,b->n,signal}; t.queued += b->n;
}
/* Decode only our bounded, canonical LogRecord encoding. This adapter has no
 * allowlist/redaction policy of its own and never sees caller payload tables.
 * Preserve repeated keys as an ordered list, just like OTLP KeyValue fields. */
static int local_varint(const unsigned char **p, const unsigned char *end,
                        uint64_t *value) {
  *value=0;
  for (unsigned shift=0; shift<64 && *p<end; shift+=7) {
    unsigned c=*(*p)++;
    if (shift==63 && c>1) return 0;
    *value |= (uint64_t)(c&127)<<shift;
    if (!(c&128)) return 1;
  }
  return 0;
}
static int local_field(const unsigned char **p, const unsigned char *end,
                       unsigned *tag, const unsigned char **value, size_t *n,
                       uint64_t *number) {
  uint64_t key, length;
  if (!local_varint(p,end,&key)) return 0;
  *tag=(unsigned)key; *value=*p; *n=0; *number=0;
  switch (key&7) {
    case 0: return local_varint(p,end,number);
    case 1: *n=8; break;
    case 2:
      if (!local_varint(p,end,&length) || length>(uint64_t)(end-*p)) return 0;
      *n=(size_t)length; *value=*p; break;
    default: return 0;
  }
  if (*n>(size_t)(end-*p)) return 0;
  *p+=*n; return 1;
}
static int local_attributes(const PB *record, LogAttribute attrs[128], size_t *count) {
  const unsigned char *p=record->p, *end=p+record->n;
  *count=0;
  while (p<end) {
    unsigned tag; const unsigned char *v; size_t n; uint64_t number;
    if (!local_field(&p,end,&tag,&v,&n,&number)) return 0;
    if (tag!=50) continue; /* LogRecord.attributes, field 6 */
    if (*count==128) return 0;
    LogAttribute *a=&attrs[(*count)++]; memset(a,0,sizeof(*a));
    const unsigned char *kv=v, *ke=v+n, *av; size_t an;
    if (!local_field(&kv,ke,&tag,&v,&n,&number) || tag!=10 || n>128) return 0;
    memcpy(a->key,v,n);
    if (!local_field(&kv,ke,&tag,&av,&an,&number) || tag!=18 || kv!=ke) return 0;
    const unsigned char *ae=av+an;
    if (!local_field(&av,ae,&tag,&v,&n,&number) || av!=ae) return 0;
    if (tag==10 && n<=128) memcpy(a->text,v,n);
    else if (tag==16) { a->type=2; a->number=number!=0; }
    else if (tag==33 && n==8) {
      uint64_t bits=0;
      for (unsigned i=0;i<8;i++) bits|=(uint64_t)v[i]<<(8*i);
      a->type=1; memcpy(&a->number,&bits,8);
    } else return 0;
  }
  return !record->bad;
}
static void lifecycle(lua_State *L, Span *s, int ended, int ok, int cancelled) {
  unsigned char buf[2*T_ATTR+1024], avbuf[64]; PB b = {buf,0,sizeof(buf),0}, av = {avbuf,0,sizeof(avbuf),0};
  fixed(&b,1,clock_ns(CLOCK_REALTIME)); num(&b,2,ended && !ok && !cancelled ? 17 : 9);
  text(L,&b,3,ended && !ok && !cancelled ? "ERROR" : "INFO");
  text(L,&av,1,ended ? "span.finished" : "span.started"); nested(&b,5,&av);
  bytes(&b,s->log_attrs,s->log_n);
  attr(L,&b,6,"span.name",s->name,0,0);
  unsigned any=0; for (int i=0;i<8;i++) any |= s->parent[i];
  if (any) {
    char parent[17]; hex(parent,s->parent,8);
    attr(L,&b,6,"parent.span_id",parent,0,0);
  }
  if (ended) {
    attributes(L,4,&b,6);
    error_attribute(L,&b,6,ok,cancelled);
    attr(L,&b,6,"cancelled",NULL,cancelled,2);
    attr(L,&b,6,"outcome",cancelled ? "cancelled" : ok ? "success" : "error",0,0);
  }
  blob(&b,9,s->trace,16); blob(&b,10,s->span,8); enqueue(&b,1);
  char trace[33], span[17]; hex(trace,s->trace,16); hex(span,s->span,8);
  /* Synchronous local lifecycle adapter only; never invoked by poll/shutdown.
   * A local write failure must not prevent span completion or OTLP export. */
  LogAttribute attrs[128]; size_t count;
  if (t.file_enabled && local_attributes(&b,attrs,&count))
    (void)log_event_structured(ended && !ok && !cancelled ? "error" : "info",
        "telemetry", ended ? "span.finished" : "span.started",
        s->session_id,trace,span,attrs,count);
}
static void finish_span(lua_State *L, Span *s, int ok, int cancelled,
                        uint64_t now);
static void startup_interval(lua_State *L, const Span *root) {
  uint64_t start=startup_time, tick=startup_tick;
  startup_time=startup_tick=0; /* One fixed slot, consumed only by a CLI root. */
  if (!start || !tick || root->tick<tick) return;
  uint64_t elapsed=root->tick-tick;
  /* Anchor to the root's wall clock: clock corrections cannot invert the
   * interval or overflow its end. Never accept caller-supplied timestamps. */
  if (elapsed>root->start) return;
  Span child=*root;
  if (!random_id(child.span,8)) return;
  memcpy(child.parent,root->span,8);
  strcpy(child.name,"operation");
  child.start=root->start-elapsed; child.tick=tick;
  int top=lua_gettop(L);
  lua_pushnil(L); lua_pushnil(L); lua_pushnil(L); lua_newtable(L);
  lua_rotate(L,1,4);
  lua_pushnumber(L,(double)elapsed/1000000.0); lua_setfield(L,4,"duration_ms");
  PB a={child.attrs,0,sizeof(child.attrs),0};
  PB logs={child.log_attrs,0,sizeof(child.log_attrs),0};
  attr(L,&a,9,"operation","startup",0,0);
  attr(L,&logs,6,"operation","startup",0,0);
  capture_attributes(L,&child.capture,&a,9);
  capture_attributes(L,&child.capture,&logs,6);
  child.n=a.n; child.log_n=logs.n;
  if (!a.bad && !logs.bad) {
    lifecycle(L,&child,0,1,0);
    finish_span(L,&child,1,0,root->tick);
  }
  lua_rotate(L,1,-4);
  lua_settop(L,top);
}
static int l_start(lua_State *L) {
  /* Identity belongs to instrumentation, not either exporter. */
  if (!permitted(L) || t.flushing) { lua_pushnil(L); return 1; }
  Span *s = NULL;
  for (size_t i=0;i<T_RECORDS;i++) if (!t.spans[i].used) { s=&t.spans[i]; break; }
  if (!s) { t.dropped++; lua_pushnil(L); return 1; }
  memset(s,0,sizeof(*s)); s->owner=L; s->context=permission(L)->generation;
  int parent = id(L,2,"trace_id",s->trace,16) && id(L,2,"span_id",s->parent,8);
  if (!parent) memset(s->parent,0,8);
  const char *session=log_session_id();
  if (session && strlen(session)<sizeof(s->session_id))
    memcpy(s->session_id,session,strlen(session)+1);
  s->capture=current_capture;
  if (capture_configured)
    memcpy(s->session_id,s->capture.session_id,sizeof(s->session_id));
  if (!capture_configured && session && session_id_valid(session))
    snprintf(s->capture.session_id,sizeof(s->capture.session_id),"%s",session);
  if (!t.include_session_name || !*s->capture.session_id) s->capture.session_name[0]=0;
  /* Identity-keyed registry snapshots survive completion without trusting
   * caller-mutated fields; weak keys release them when deferred owners do. */
  if (lua_istable(L,2)) {
    captures(L); lua_pushvalue(L,2); lua_rawget(L,-2);
    if (lua_isuserdata(L,-1)) {
      const Capture *saved=lua_touserdata(L,-1);
      s->capture=*saved;
      memcpy(s->trace,saved->trace,16); memcpy(s->parent,saved->span,8); parent=1;
      memcpy(s->session_id,saved->session_id,sizeof(s->session_id));
    }
    lua_pop(L,2);
  }
  if ((!parent && !random_id(s->trace,16)) || !random_id(s->span,8)) { lua_pushnil(L); return 1; }
  const char *names[] = {"agent.run","agent.model","agent.tool","run","model","tool","subagent","compaction","title","completion_review",NULL};
  strcpy(s->name,"operation");
  size_t n=0; const char *name = lua_type(L,1)==LUA_TSTRING ? lua_tolstring(L,1,&n) : NULL;
  for (int i=0;name && names[i];i++) if (strlen(names[i])==n && !memcmp(name,names[i],n)) strcpy(s->name,names[i]);
  if (!*s->capture.run_id) hex(s->capture.run_id,s->span,8);
  memcpy(s->capture.trace,s->trace,16); memcpy(s->capture.span,s->span,8);
  PB a={s->attrs,0,sizeof(s->attrs),0}; attributes(L,3,&a,9);
  capture_attributes(L,&s->capture,&a,9);
  PB logs={s->log_attrs,0,sizeof(s->log_attrs),0}; attributes(L,3,&logs,6);
  capture_attributes(L,&s->capture,&logs,6);
  if (a.bad || logs.bad) { t.dropped++; lua_pushnil(L); return 1; }
  s->n=a.n; s->log_n=logs.n;
  s->start=clock_ns(CLOCK_REALTIME); s->tick=clock_ns(CLOCK_MONOTONIC);
  char trace[33], span[17]; hex(trace,s->trace,16); hex(span,s->span,8);
  lua_createtable(L,0,3); lua_pushstring(L,trace); lua_setfield(L,-2,"trace_id");
  lua_pushstring(L,span); lua_setfield(L,-2,"span_id");
  lua_pushstring(L,s->session_id); lua_setfield(L,-2,"session_id");
  lua_pushstring(L,s->capture.run_id); lua_setfield(L,-2,"run_id");
  int result=lua_gettop(L);
  captures(L); lua_pushvalue(L,result);
  Capture *saved=lua_newuserdatauv(L,sizeof(*saved),0); *saved=s->capture;
  lua_rawset(L,-3); lua_pop(L,1);
  s->used=1; lifecycle(L,s,0,1,0);
  if (!parent && !strcmp(s->name,"agent.run") &&
      !strcmp(s->capture.mode,"cli")) startup_interval(L,s);
  return 1;
}
static int l_end(lua_State *L) {
  unsigned char trace[16], span[8]; Span *s=NULL;
  int valid=id(L,1,"trace_id",trace,16) && id(L,1,"span_id",span,8);
  if (lua_istable(L,1)) {
    captures(L); lua_pushvalue(L,1); lua_rawget(L,-2);
    if (lua_isuserdata(L,-1)) {
      const Capture *saved=lua_touserdata(L,-1);
      memcpy(trace,saved->trace,16); memcpy(span,saved->span,8); valid=1;
    }
    lua_pop(L,2);
  }
  if (permitted(L) && !t.flushing && valid)
    for (size_t i=0;i<T_RECORDS;i++) if (t.spans[i].used && t.spans[i].owner==L &&
        t.spans[i].context==permission(L)->generation &&
        !memcmp(trace,t.spans[i].trace,16) && !memcmp(span,t.spans[i].span,8)) { s=&t.spans[i]; break; }
  if (!s) { lua_pushboolean(L,0); return 1; }
  int ok=lua_isboolean(L,2) && lua_toboolean(L,2), cancelled=lua_isboolean(L,3) && lua_toboolean(L,3);
  finish_span(L,s,ok,cancelled,clock_ns(CLOCK_MONOTONIC));
  lua_pushboolean(L,1); return 1;
}
static void finish_span(lua_State *L, Span *s, int ok, int cancelled,
                        uint64_t now) {
  unsigned char buf[2*T_ATTR+512], statusbuf[8]; PB b={buf,0,sizeof(buf),0}, status={statusbuf,0,sizeof(statusbuf),0};
  blob(&b,1,s->trace,16); blob(&b,2,s->span,8);
  unsigned any=0; for (int i=0;i<8;i++) any |= s->parent[i];
  if (any) blob(&b,4,s->parent,8);
  text(L,&b,5,s->name); num(&b,6,1); fixed(&b,7,s->start);
  fixed(&b,8,s->start+(now>=s->tick ? now-s->tick : 0));
  bytes(&b,s->attrs,s->n); attributes(L,4,&b,9);
  error_attribute(L,&b,9,ok,cancelled);
  attr(L,&b,9,"cancelled",NULL,cancelled,2);
  attr(L,&b,9,"outcome",cancelled ? "cancelled" : ok ? "success" : "error",0,0);
  num(&status,3,cancelled ? 0 : ok ? 1 : 2); nested(&b,15,&status);
  s->used=0; enqueue(&b,0); lifecycle(L,s,1,ok,cancelled);
}
static size_t receive(char *p,size_t a,size_t b,void *unused) {
  (void)unused; if (a && b>SIZE_MAX/a) return 0; size_t n=a*b;
  if (n>sizeof(t.response)-t.response_n) return 0;
  memcpy(t.response+t.response_n,p,n); t.response_n+=n; return n;
}
static size_t header(char *p,size_t a,size_t b,void *unused) {
  (void)unused; if (a && b>SIZE_MAX/a) return 0; size_t n=a*b;
  if (n>=5 && !memcmp(p,"HTTP/",5)) t.retry_after=0;
  if (n>12 && n<256 && !strncasecmp(p,"Retry-After:",12)) {
    char value[256]; memcpy(value,p+12,n-12); value[n-12]=0;
    char *s=value; while (*s==' ' || *s=='\t') s++;
    char *end; double seconds=strtod(s,&end);
    while (*end=='\r' || *end=='\n' || *end==' ' || *end=='\t') end++;
    if (end==s || *end || !isfinite(seconds) || seconds<0) {
      time_t date=curl_getdate(s,NULL); seconds=date<0 ? 0 : difftime(date,time(NULL));
    }
    if (seconds<0) seconds=0;
    if (seconds>60) seconds=60;
    t.retry_after=(uint64_t)(seconds*1000);
  } return n;
}
static void release_easy(void) {
  if (t.easy) { curl_multi_remove_handle(t.multi,t.easy); curl_easy_cleanup(t.easy); t.easy=NULL; }
}
static void deliver(void) {
  t.easy=curl_easy_init(); t.attempts++; t.response_n=0; t.retry_after=0;
  if (!t.easy) { t.error_category="exporter_transport"; t.failed+=t.batch; t.batch=0; t.payload_n=0; return; }
#define OPT(k,v) do { if (curl_easy_setopt(t.easy,k,v)!=CURLE_OK) goto bad; } while (0)
  OPT(CURLOPT_URL,t.endpoint[t.signal]); OPT(CURLOPT_HTTPHEADER,t.headers[t.signal]);
  OPT(CURLOPT_POST,1L); OPT(CURLOPT_POSTFIELDS,t.payload);
  OPT(CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)t.payload_n);
  OPT(CURLOPT_TIMEOUT_MS,5000L); OPT(CURLOPT_CONNECTTIMEOUT_MS,5000L);
  OPT(CURLOPT_NOSIGNAL,1L); OPT(CURLOPT_FOLLOWLOCATION,0L);
  OPT(CURLOPT_SSL_VERIFYPEER,1L); OPT(CURLOPT_SSL_VERIFYHOST,2L);
  OPT(CURLOPT_PROXY_SSL_VERIFYPEER,1L); OPT(CURLOPT_PROXY_SSL_VERIFYHOST,2L);
  OPT(CURLOPT_NETRC,(long)CURL_NETRC_IGNORED);
#if LIBCURL_VERSION_NUM >= 0x075500
  OPT(CURLOPT_PROTOCOLS_STR,"http,https");
#else
  OPT(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
  OPT(CURLOPT_WRITEFUNCTION,receive); OPT(CURLOPT_HEADERFUNCTION,header);
  if (curl_multi_add_handle(t.multi,t.easy)==CURLM_OK) return;
bad:
  release_easy(); t.error_category="exporter_transport"; t.failed+=t.batch; t.batch=0; t.payload_n=0;
#undef OPT
}
static void batch(void) {
  PB resource={t.resource,t.resource_n,sizeof(t.resource),0},
     scope={t.scope,t.scope_n,sizeof(t.scope),0};
  unsigned char *inner=malloc(T_PAYLOAD), *outer=malloc(T_PAYLOAD);
  if (!inner || !outer) {
    free(inner); free(outer);
    Record r=t.queue[0]; t.queued-=r.n; free(r.p);
    memmove(t.queue,t.queue+1,(--t.count)*sizeof(Record)); t.dropped++;
    t.due=mono()+1000; return;
  }
  PB records={inner,0,T_PAYLOAD,0}, rs={outer,0,T_PAYLOAD,0};
  nested(&records,1,&scope); t.signal=t.queue[0].signal; t.batch=0;
  /* Shutdown has one shared deadline for both signals. Lifecycle logs usually
   * lead the queue; a slow logs receiver must not consume the entire budget
   * before completed spans (especially the last root) get an export attempt.
   * Preserve in-flight requests/retry backoff and ordering within each signal. */
  if (t.flushing)
    for (size_t i=0;i<t.count;i++) if (t.queue[i].signal==0) {
      t.signal=0; break;
    }
  /* Gather one signal across interleaved lifecycle/span records, preserving
   * order within each signal and leaving the other signal queued. */
  size_t kept=0;
  for (size_t i=0;i<t.count;i++) {
    Record r=t.queue[i];
    if (r.signal==t.signal && t.batch<128 &&
        records.n+r.n+t.resource_n+256<T_PAYLOAD) {
      blob(&records,2,r.p,r.n); t.queued-=r.n; free(r.p); t.batch++;
    } else t.queue[kept++]=r;
  }
  t.count=kept;
  nested(&rs,1,&resource); nested(&rs,2,&records);
  PB request={t.payload,0,sizeof(t.payload),0}; nested(&request,1,&rs);
  free(inner); free(outer);
  if (request.bad) { t.dropped+=t.batch; return; }
  t.payload_n=request.n; t.attempts=0; deliver();
}
/* Pure result policy: transport errors outrank HTTP status; never inspect text.
 * Success leaves the latest failure untouched in the diagnostics adapter. */
const char *telemetry_exporter_result_category(int curl_code, long http_status) {
  switch (curl_code) {
    case CURLE_OK: break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY: return "exporter_dns";
    case CURLE_COULDNT_CONNECT: return "exporter_connect";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_USE_SSL_FAILED:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS: return "exporter_tls";
    case CURLE_LOGIN_DENIED: return "exporter_auth";
    default: return "exporter_transport";
  }
  if (http_status==401 || http_status==403 || http_status==407)
    return "exporter_auth";
  return http_status==200 ? NULL : "exporter_http";
}
/* Diagnostics are read explicitly through Lua; polling never writes logs. */
void telemetry_poll(void) {
  if (!t.enabled) return;
  uint64_t now=mono();
  if (!t.easy && now>=t.due) {
    if (t.payload_n) deliver();
    else if (t.count) batch();
    else t.due=now+1000;
  }
  int running;
  if (curl_multi_perform(t.multi,&running)!=CURLM_OK) {
    release_easy(); t.error_category="exporter_transport"; t.failed+=t.batch; t.batch=0; t.payload_n=0; t.due=now+1000; return;
  }
  int left; CURLMsg *msg;
  while ((msg=curl_multi_info_read(t.multi,&left))) {
    if (msg->msg!=CURLMSG_DONE) continue;
    CURLcode code=msg->data.result; long status=0;
    curl_easy_getinfo(t.easy,CURLINFO_RESPONSE_CODE,&status); release_easy();
    int connection=code==CURLE_COULDNT_CONNECT || code==CURLE_COULDNT_RESOLVE_HOST || code==CURLE_COULDNT_RESOLVE_PROXY || code==CURLE_SEND_ERROR || code==CURLE_RECV_ERROR || code==CURLE_GOT_NOTHING || code==CURLE_OPERATION_TIMEDOUT;
    int retry=connection || (code==CURLE_OK && (status==429 || status==502 || status==503 || status==504));
    if (retry && t.attempts<3) {
      uint64_t wait=(uint64_t)1000<<(t.attempts-1);
      if (t.retry_after>wait) wait=t.retry_after;
      t.due=now+wait;
    } else {
      if (code==CURLE_OK && status==200) {
        uint64_t rejected=0;
        if (!otlp_wire_response(t.response,t.response_n,&rejected)) {
          t.malformed++; t.error_category="exporter_protocol";
        } else {
          t.rejected+=rejected>t.batch ? t.batch : rejected;
          if (rejected) t.error_category="exporter_rejected";
        }
      } else {
        t.failed+=t.batch;
        t.error_category=telemetry_exporter_result_category(code,status);
      }
      t.payload_n=0; t.batch=0; t.due=t.count ? now : now+1000;
    }
  }
}
/* Empty environment variables are unset, as required by the OTel SDK spec. */
static const char *environment(const char *name) {
  const char *s=name ? getenv(name) : NULL;
  return s && *s ? s : NULL;
}
static int setting(lua_State *L,int config,const char *key,const char *env,char *out,size_t cap,const char *fallback) {
  const char *s=environment(env); size_t n=0; int pushed=0;
  if (!s && config) {
    field(L,config,key); pushed=1;
    if (!lua_isnil(L,-1)) {
      if (lua_type(L,-1)!=LUA_TSTRING) { lua_pop(L,1); return 0; }
      s=lua_tolstring(L,-1,&n);
      if (n>=cap || memchr(s,0,n)) { lua_pop(L,1); return 0; }
    }
  }
  if (!s) s=fallback;
  if (!pushed || !n) n=strnlen(s,cap);
  int ok=n<cap;
  if (ok) { memcpy(out,s,n); out[n]=0; }
  if (pushed) lua_pop(L,1);
  return ok;
}
/* All environment settings outrank all config settings. Signal-specific wins
 * within a source. A generic env endpoint is a base, not a full signal URL. */
static int signal_setting(lua_State *L,int config,const char *key,const char *env,
                          const char *generic_key,const char *generic_env,
                          char *out,size_t cap,const char *fallback) {
  const char *s=environment(env);
  if (!s) s=environment(generic_env);
  if (s) {
    size_t n=strnlen(s,cap); if (n==cap) return 0;
    memcpy(out,s,n+1); return 1;
  }
  if (config) {
    field(L,config,key); int present=!lua_isnil(L,-1); lua_pop(L,1);
    if (present) return setting(L,config,key,NULL,out,cap,fallback);
  }
  return setting(L,config,generic_key,NULL,out,cap,fallback);
}
static int endpoint_valid(const char *s) {
  size_t prefix=!strncasecmp(s,"https://",8) ? 8 : !strncasecmp(s,"http://",7) ? 7 : 0;
  if (!prefix || !s[prefix] || s[prefix]=='/' || s[prefix]=='?') return 0;
  for (const unsigned char *p=(const unsigned char *)s;*p;p++)
    if (*p<=32 || *p>=127 || *p=='#' || *p=='@' || *p=='\\') return 0;
  CURLU *url=curl_url(); if (!url) return 0;
  int ok=curl_url_set(url,CURLUPART_URL,s,0)==CURLUE_OK;
  curl_url_cleanup(url); return ok;
}
typedef struct { char key[129], value[2049]; } Pair;
typedef struct { Pair pairs[T_PAIRS]; size_t n, bytes; } Pairs;
static int put_pair(Pairs *ps,const char *key,size_t kn,const char *value,size_t vn,int headers) {
  if (!kn || kn>128 || vn>(headers ? 2048u : 128u) ||
      kn+vn>T_SETTINGS-ps->bytes || memchr(key,0,kn) || memchr(value,0,vn)) return 0;
  for (size_t i=0;i<kn;i++) {
    unsigned char c=(unsigned char)key[i];
    if (headers ? !((c>='a' && c<='z') || (c>='A' && c<='Z') ||
        (c>='0' && c<='9') || strchr("!#$%&'*+-.^_`|~",c)) : (c<33 || c>=127)) return 0;
  }
  if (headers) for (size_t i=0;i<vn;i++)
    if ((unsigned char)value[i]<32 || (unsigned char)value[i]>=127) return 0;
  size_t at=0;
  for (;at<ps->n;at++) if (strlen(ps->pairs[at].key)==kn &&
      (headers ? !strncasecmp(ps->pairs[at].key,key,kn) : !memcmp(ps->pairs[at].key,key,kn))) break;
  if (at==T_PAIRS) return 0;
  if (at==ps->n) ps->n++;
  Pair *p=&ps->pairs[at]; memcpy(p->key,key,kn); p->key[kn]=0;
  memcpy(p->value,value,vn); p->value[vn]=0; ps->bytes+=kn+vn;
  return 1;
}
static int unhex(unsigned char c) {
  return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : -1;
}
static int decode(char *out,size_t cap,const char *s,size_t n,size_t *length) {
  size_t used=0;
  for (size_t i=0;i<n;i++) {
    unsigned char c=(unsigned char)s[i];
    if (c=='%') {
      if (n-i<3 || unhex(s[i+1])<0 || unhex(s[i+2])<0) return 0;
      c=(unsigned char)(16*unhex(s[i+1])+unhex(s[i+2])); i+=2;
    }
    if (!c || used==cap-1) return 0;
    out[used++]=(char)c;
  }
  out[used]=0; *length=used; return 1;
}
static int parse_pairs(Pairs *ps,const char *s,size_t n,int headers) {
  if (n>T_SETTINGS || memchr(s,0,n)) return 0;
  size_t at=0;
  while (at<n) {
    size_t end=at; while (end<n && s[end]!=',') end++;
    size_t first=at,last=end;
    while (first<last && (s[first]==' ' || s[first]=='\t')) first++;
    while (last>first && (s[last-1]==' ' || s[last-1]=='\t')) last--;
    size_t eq=first; while (eq<last && s[eq]!='=') eq++;
    if (eq==last) return 0;
    size_t ke=eq,vs=eq+1;
    while (ke>first && (s[ke-1]==' ' || s[ke-1]=='\t')) ke--;
    while (vs<last && (s[vs]==' ' || s[vs]=='\t')) vs++;
    char key[129],value[2049]; size_t kn,vn;
    if (!decode(key,sizeof(key),s+first,ke-first,&kn) ||
        !decode(value,sizeof(value),s+vs,last-vs,&vn) || !put_pair(ps,key,kn,value,vn,headers)) return 0;
    at=end<n ? end+1 : end;
    if (at==n && end<n) return 0;
  }
  return 1;
}
/* Config accepts a string map or the same percent-encoded key=value list as env.
 * Raw table iteration invokes no user metamethods. */
static int config_pairs(lua_State *L,int config,const char *key,Pairs *ps,int headers) {
  if (!config) return 1;
  int top=lua_gettop(L),ok=1; field(L,config,key);
  if (lua_type(L,-1)==LUA_TSTRING) {
    size_t n; const char *s=lua_tolstring(L,-1,&n); ok=parse_pairs(ps,s,n,headers);
  } else if (lua_istable(L,-1)) {
    lua_pushnil(L);
    while (lua_next(L,-2)) {
      if (lua_type(L,-2)!=LUA_TSTRING || lua_type(L,-1)!=LUA_TSTRING) { ok=0; break; }
      size_t kn,vn; const char *k=lua_tolstring(L,-2,&kn),*v=lua_tolstring(L,-1,&vn);
      if (!put_pair(ps,k,kn,v,vn,headers)) { ok=0; break; }
      lua_pop(L,1);
    }
  } else if (!lua_isnil(L,-1)) ok=0;
  lua_settop(L,top); return ok;
}
static int append_header(int signal,const char *line) {
  struct curl_slist *next=curl_slist_append(t.headers[signal],line);
  if (!next) return 0;
  t.headers[signal]=next; return 1;
}
static int configure_headers(lua_State *L,int config,int signal,Pairs *ps) {
  memset(ps,0,sizeof(*ps));
  const char *env=environment(signal ? "OTEL_EXPORTER_OTLP_LOGS_HEADERS" : "OTEL_EXPORTER_OTLP_TRACES_HEADERS");
  if (!env) env=environment("OTEL_EXPORTER_OTLP_HEADERS");
  if (env) {
    if (!parse_pairs(ps,env,strnlen(env,T_SETTINGS+1),1)) return 0;
  } else {
    const char *key=signal ? "logs_headers" : "traces_headers";
    field(L,config,key); int present=!lua_isnil(L,-1); lua_pop(L,1);
    if (!config_pairs(L,config,present ? key : "headers",ps,1)) return 0;
  }
  for (size_t i=0;i<ps->n;i++) {
    Pair *p=&ps->pairs[i];
    /* Transport-owned fields cannot be overridden by credential configuration. */
    const char *reserved[]={"content-type","content-length","transfer-encoding","host","connection","expect","trailer","te",NULL};
    for (int j=0;reserved[j];j++) if (!strcasecmp(p->key,reserved[j])) return 0;
    char line[2182];
    int n=snprintf(line,sizeof(line),p->value[0] ? "%s: %s" : "%s;%s",p->key,p->value);
    if (n<0 || (size_t)n>=sizeof(line) || !append_header(signal,line)) return 0;
  }
  return append_header(signal,"Content-Type: application/x-protobuf");
}
static int configure_resource(lua_State *L,int config,Pairs *ps) {
  memset(ps,0,sizeof(*ps));
  const char *env=environment("OTEL_RESOURCE_ATTRIBUTES");
  if (env) {
    if (!parse_pairs(ps,env,strnlen(env,T_SETTINGS+1),0)) return 0;
  } else if (!config_pairs(L,config,"resource_attributes",ps,0)) return 0;
  if (!setting(L,config,"service_name","OTEL_SERVICE_NAME",t.service,sizeof(t.service),"capstan") ||
      !setting(L,config,"service_version",NULL,t.version,sizeof(t.version),APP_VERSION_VALUE)) return 0;
  for (size_t i=0;i<ps->n;i++) {
    if (!strcmp(ps->pairs[i].key,"service.name") && !environment("OTEL_SERVICE_NAME"))
      memcpy(t.service,ps->pairs[i].value,strlen(ps->pairs[i].value)+1);
    if (!strcmp(ps->pairs[i].key,"service.version"))
      memcpy(t.version,ps->pairs[i].value,strlen(ps->pairs[i].value)+1);
  }
  if (!put_pair(ps,"service.name",12,t.service,strlen(t.service),0) ||
      !put_pair(ps,"service.version",15,t.version,strlen(t.version),0)) return 0;
  PB b={t.resource,0,sizeof(t.resource),0};
  for (size_t i=0;i<ps->n;i++) {
    attr(L,&b,1,ps->pairs[i].key,ps->pairs[i].value,0,0);
  }
  t.resource_n=b.n; return !b.bad;
}
void telemetry_init(lua_State *L,int isolated) {
  if (!L) return;
  int top=lua_gettop(L); lua_getglobal(L,"capstan");
  if (!lua_istable(L,-1)) { lua_pop(L,1); lua_newtable(L); }
  int capstan=lua_gettop(L);
  /* Registry lifetime prevents a recycled lua_State address from inheriting
   * span ownership. All registrations share sticky isolation, including saved
   * closures from before the state was marked isolated. */
  lua_rawgetp(L,LUA_REGISTRYINDEX,&context_key);
  Context *c=lua_touserdata(L,-1);
  if (!c) {
    lua_pop(L,1); c=lua_newuserdatauv(L,sizeof(*c),0);
    c->allowed=!isolated; c->generation=++context_generation;
    lua_pushvalue(L,-1); lua_rawsetp(L,LUA_REGISTRYINDEX,&context_key);
  }
  if (isolated) c->allowed=0;
  int context=lua_gettop(L);
  lua_newtable(L);
  lua_pushvalue(L,context); lua_pushcclosure(L,l_start,1); lua_setfield(L,-2,"start");
  lua_pushvalue(L,context); lua_pushcclosure(L,l_end,1); lua_setfield(L,-2,"end_span");
  lua_pushvalue(L,context); lua_pushcclosure(L,l_diagnostics,1); lua_setfield(L,-2,"diagnostics");
  lua_setfield(L,capstan,"telemetry");
  lua_pushvalue(L,capstan); lua_setglobal(L,"capstan");
  if (!c->allowed || t.initialized) { lua_settop(L,top); return; }
  t.initialized=1;
  include_tool_details=0;
  const char *disabled=getenv("OTEL_SDK_DISABLED");
  if (disabled && !strcasecmp(disabled,"true")) goto done;
  field(L,capstan,"config"); int config=0;
  if (lua_istable(L,-1)) { field(L,-1,"observability"); if (lua_istable(L,-1)) config=lua_gettop(L); }
  /* The local adapter follows the existing log destination/level policy.
   * SDK disable suppresses both sinks, but never revokes local identity. */
  t.file_enabled=1;
  if (!config) goto done;
  field(L,config,"file_exporter");
  if (!lua_isnil(L,-1) && !lua_isboolean(L,-1)) {
    lua_pop(L,1); t.file_enabled=0; goto invalid;
  }
  if (lua_isboolean(L,-1)) t.file_enabled=lua_toboolean(L,-1);
  lua_pop(L,1);
  field(L,config,"include_tool_details");
  if (!lua_isnil(L,-1) && !lua_isboolean(L,-1)) { lua_pop(L,1); goto invalid; }
  include_tool_details=lua_isboolean(L,-1) && lua_toboolean(L,-1); lua_pop(L,1);
  field(L,config,"include_session_name");
  if (!lua_isnil(L,-1) && !lua_isboolean(L,-1)) { lua_pop(L,1); goto invalid; }
  t.include_session_name=lua_toboolean(L,-1); lua_pop(L,1);
  field(L,config,"enabled"); int enabled=lua_isboolean(L,-1) && lua_toboolean(L,-1); lua_pop(L,1);
  if (!enabled) goto done;
  char base[2048], fallback[2048], raw[129];
  /* Own a balanced global reference; URL validation also uses libcurl. */
  if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) goto invalid;
  t.global_curl=1;
  const curl_version_info_data *curl_info=curl_version_info(CURLVERSION_NOW);
  if (!curl_info || !(curl_info->features & CURL_VERSION_ASYNCHDNS)) goto invalid;
  Pairs pairs;
  for (int i=0;i<2;i++) {
    if (!setting(L,config,i ? "logs_exporter" : "traces_exporter",
        i ? "OTEL_LOGS_EXPORTER" : "OTEL_TRACES_EXPORTER",raw,sizeof(raw),"otlp")) goto invalid;
    if (!strcmp(raw,"none")) continue;
    if (strcmp(raw,"otlp")) goto invalid;
    t.signals[i]=1;
    if (!signal_setting(L,config,i ? "logs_protocol" : "traces_protocol",
        i ? "OTEL_EXPORTER_OTLP_LOGS_PROTOCOL" : "OTEL_EXPORTER_OTLP_TRACES_PROTOCOL",
        "protocol","OTEL_EXPORTER_OTLP_PROTOCOL",raw,sizeof(raw),"http/protobuf") || strcmp(raw,"http/protobuf")) goto invalid;
    const char *specific=environment(i ? "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT" : "OTEL_EXPORTER_OTLP_TRACES_ENDPOINT");
    const char *key=i ? "logs_endpoint" : "traces_endpoint";
    field(L,config,key); int configured=!lua_isnil(L,-1); lua_pop(L,1);
    if (specific || (!environment("OTEL_EXPORTER_OTLP_ENDPOINT") && configured)) {
      if (!setting(L,config,key,i ? "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT" : "OTEL_EXPORTER_OTLP_TRACES_ENDPOINT",
          t.endpoint[i],sizeof(t.endpoint[i]),"")) goto invalid;
    } else {
      if (!setting(L,config,"endpoint","OTEL_EXPORTER_OTLP_ENDPOINT",base,sizeof(base),"http://localhost:4318") || !endpoint_valid(base)) goto invalid;
      /* Append the signal path before a query, preserving explicit query data. */
      char *query=strchr(base,'?'); size_t len=query ? (size_t)(query-base) : strlen(base);
      while (len && base[len-1]=='/') len--;
      int written=snprintf(fallback,sizeof(fallback),"%.*s/v1/%s%s",(int)len,base,i ? "logs" : "traces",query ? query : "");
      if (written<0 || (size_t)written>=sizeof(fallback)) goto invalid;
      memcpy(t.endpoint[i],fallback,(size_t)written+1);
    }
    if (!endpoint_valid(t.endpoint[i]) || !configure_headers(L,config,i,&pairs)) goto invalid;
  }
  if (!t.signals[0] && !t.signals[1]) goto done;
  if (!configure_resource(L,config,&pairs)) goto invalid;
  PB scope={t.scope,0,sizeof(t.scope),0};
  text(L,&scope,1,"capstan.native"); text(L,&scope,2,"1");
  if (scope.bad) goto invalid;
  t.scope_n=scope.n;
  t.multi=curl_multi_init();
  if (!t.multi) goto invalid;
  t.enabled=1; t.due=mono()+1000; goto done;
invalid:
  if (t.multi) { curl_multi_cleanup(t.multi); t.multi=NULL; }
  for (int i=0;i<2;i++) { curl_slist_free_all(t.headers[i]); t.headers[i]=NULL; t.signals[i]=0; }
  if (t.global_curl) { curl_global_cleanup(); t.global_curl=0; }
  t.configuration_errors++; t.error_category="exporter_configuration";
done:
  if (t.enabled && !isolated && startup_time &&
      strcmp(current_capture.mode,"cli")) {
    unsigned char storage[256], body_storage[128];
    PB record={storage,0,sizeof(storage),0}, body={body_storage,0,sizeof(body_storage),0};
    fixed(&record,1,startup_time); num(&record,2,9);
    text(L,&record,3,"INFO"); text(L,&body,1,"runtime.started");
    nested(&record,5,&body); enqueue(&record,1);
  }
  if (strcmp(current_capture.mode,"cli")) startup_time=startup_tick=0;
  lua_settop(L,top);
}
void telemetry_cleanup(void) {
  startup_time=startup_tick=0;
  t.flushing=1; uint64_t deadline=mono()+2000;
  if (!t.payload_n) t.due=0;
  while (t.enabled && (t.count || t.payload_n || t.easy) && mono()<deadline) {
    telemetry_poll();
    if (t.easy) { int ready; curl_multi_wait(t.multi,NULL,0,10,&ready); }
    else { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
  }
  release_easy();
  for (size_t i=0;i<t.count;i++) { free(t.queue[i].p); t.dropped++; }
  if (t.payload_n) t.dropped+=t.batch;
  for (size_t i=0;i<T_RECORDS;i++) if (t.spans[i].used) {
    t.dropped++; t.spans[i].used=0;
  }
  if (t.multi) curl_multi_cleanup(t.multi);
  for (int i=0;i<2;i++) { curl_slist_free_all(t.headers[i]); t.headers[i]=NULL; t.signals[i]=0; }
  if (t.global_curl) curl_global_cleanup();
  /* Shutdown-only losses remain in counters: logging here could defeat the
   * deadline. A nonblocking, session-aware local adapter needs log API work. */
  t.multi=NULL; t.easy=NULL; t.global_curl=0; t.enabled=0;
  t.count=t.queued=t.payload_n=0;
}
