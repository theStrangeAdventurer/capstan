#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>
#include <curl/typecheck-gcc.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <ncursesw/curses.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "http.h"
#include "popup.h"
#include "tui.h"

typedef struct {
  char *data;
  size_t size;
  size_t cap;
  size_t max_size;
  int overflow;
} RespBuf;

typedef struct {
  CURL *easy;
  lua_State *L;
  int callback_ref;
  struct curl_slist *headers;
  char *body;
  size_t body_len;
  int response_mode;
  int background;
  int async_id;
  RespBuf response;
  RespBuf response_headers;
  char err_buf[4096 + 1];
  size_t err_len;
} StreamCtx;

static CURLM *multi_handle = NULL;
static StreamCtx **streams = NULL;
static int stream_count = 0;
static int stream_cap = 0;
static int g_next_async_id = 1;
static int g_sync_active = 0;
static int g_headless = 0;
static long long g_last_wait_render_ms = 0;

#define HTTP_WAIT_RENDER_INTERVAL_MS 16
#define HTTP_DEFAULT_TIMEOUT_MS 60000L
#define HTTP_CONNECT_TIMEOUT_MS 10000L
#define HTTP_LOW_SPEED_LIMIT 1L
#define HTTP_LOW_SPEED_TIME 60L
#define HTTP_MAX_RESPONSE_BYTES (10L * 1024L * 1024L)
#define HTTP_MAX_HEADER_BYTES (256L * 1024L)

int http_is_loading(void) {
  if (g_sync_active)
    return 1;
  for (int i = 0; i < stream_count; i++) {
    if (streams[i] && !streams[i]->background)
      return 1;
  }
  return 0;
}

int http_has_background_work(void) {
  for (int i = 0; i < stream_count; i++) {
    if (streams[i] && streams[i]->background)
      return 1;
  }
  return 0;
}

void http_set_headless(int headless) { g_headless = headless; }

static long long http_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void http_wait_frame(void) {
  if (g_headless) {
    usleep(10000);
  } else {
    napms(10);
    long long now = http_now_ms();
    if (g_last_wait_render_ms == 0 ||
        now - g_last_wait_render_ms >= HTTP_WAIT_RENDER_INTERVAL_MS) {
      render_all();
      g_last_wait_render_ms = now;
    }
  }
}

/*
 * CURLOPT_WRITEFUNCTION callback.
 * libcurl may invoke it multiple times per request, once per received chunk.
 * It must return the exact number of consumed bytes or curl will fail the
 * transfer.
 */
static size_t
write_cb(char *chunk_ptr,
         size_t size,
         size_t count,
         void *resp_data_ptr
) {
  RespBuf *buf = resp_data_ptr;
  size_t total = size * count;
  size_t max_size = buf->max_size ? buf->max_size : HTTP_MAX_RESPONSE_BYTES;
  if (total > max_size || buf->size + total > max_size) {
    buf->overflow = 1;
    return 0;
  }
  char *new_ptr = realloc(buf->data, buf->size + total + 1);

  if (!new_ptr)
    return 0;

  buf->data = new_ptr;
  memcpy(buf->data + buf->size, chunk_ptr, total);
  buf->size += total;
  buf->data[buf->size] = '\0';

  return total;
}

static struct curl_slist *parse_headers(lua_State *L, int index) {
  struct curl_slist *headers = NULL;

  if (lua_istable(L, index)) {
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
      const char *key = lua_tostring(L, -2);
      const char *val = lua_tostring(L, -1);
      if (key && val) {
        size_t header_len = strlen(key) + 2 + strlen(val) + 1;
        char *header = malloc(header_len);
        if (!header) {
          lua_pop(L, 1);
          curl_slist_free_all(headers);
          return NULL;
        }
        snprintf(header, header_len, "%s: %s", key, val);
        headers = curl_slist_append(headers, header);
        free(header);
      }
      lua_pop(L, 1);
    }
  }
  return headers;
}

static int lua_opt_background(lua_State *L, int index) {
  if (!lua_istable(L, index))
    return 0;
  lua_getfield(L, index, "background");
  int background = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return background;
}

static void push_headers_table(lua_State *L, const char *raw_headers) {
  lua_newtable(L);
  if (!raw_headers)
    return;

  const char *line = raw_headers;
  while (*line) {
    const char *end = strstr(line, "\r\n");
    size_t len = end ? (size_t)(end - line) : strlen(line);
    if (len > 0) {
      const char *colon = memchr(line, ':', len);
      if (colon && colon > line) {
        size_t key_len = (size_t)(colon - line);
        size_t val_start = key_len + 1;
        while (val_start < len &&
               (line[val_start] == ' ' || line[val_start] == '\t')) {
          val_start++;
        }

        char key[256];
        size_t copy_key = key_len < sizeof(key) - 1 ? key_len : sizeof(key) - 1;
        for (size_t i = 0; i < copy_key; i++) {
          char c = line[i];
          if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
          key[i] = c;
        }
        key[copy_key] = '\0';

        lua_pushlstring(L, line + val_start, len - val_start);
        lua_setfield(L, -2, key);
      }
    }
    if (!end)
      break;
    line = end + 2;
  }
}

static void configure_http_handle(CURL *easy, const char *url) {
  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS);
  curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, HTTP_LOW_SPEED_LIMIT);
  curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, HTTP_LOW_SPEED_TIME);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
}

static void stream_ctx_free(StreamCtx *ctx) {
  if (!ctx)
    return;
  free(ctx->body);
  free(ctx->response.data);
  free(ctx->response_headers.data);
  free(ctx);
}

/* Streaming callback: forwards raw bytes to the Lua handler immediately. */
static size_t stream_write_cb(char *chunk_ptr, size_t size, size_t count,
                              void *userdata) {
  StreamCtx *ctx = userdata;
  size_t total = size * count;
  if (total == 0)
    return 0;

  if (ctx->response_mode)
    return write_cb(chunk_ptr, size, count, &ctx->response);

  size_t room = sizeof(ctx->err_buf) - 1 - ctx->err_len;
  if (room > 0) {
    size_t copy = total < room ? total : room;
    memcpy(ctx->err_buf + ctx->err_len, chunk_ptr, copy);
    ctx->err_len += copy;
    ctx->err_buf[ctx->err_len] = '\0';
  }

  lua_getglobal(ctx->L, "debug");
  lua_getfield(ctx->L, -1, "traceback");
  lua_remove(ctx->L, -2);
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
  lua_pushlstring(ctx->L, chunk_ptr, total);
  lua_pushboolean(ctx->L, 0);

  int msgh = lua_gettop(ctx->L) - 3;
  if (lua_pcall(ctx->L, 2, 0, msgh) != LUA_OK) {
    if (g_headless)
      fprintf(stderr, "capstan stream error: %s\n", lua_tostring(ctx->L, -1));
    else
      popup_show_message("Stream Error", lua_tostring(ctx->L, -1), 1);
    lua_settop(ctx->L, msgh - 1);
    return 0;
  }
  lua_settop(ctx->L, msgh - 1);
  return total;
}

/*
 * http.post_stream(url, body, headers, callback[, timeout_ms[, options]])
 * callback(raw_chunk, is_done) is invoked for every received chunk. A failed
 * transfer finishes with callback(nil, true, error, body_or_nil, headers).
 * options.background keeps metadata/background streams out of the visible
 * loading state and user-triggered stream cancellation.
 * Returns async_id.
 */
static int l_http_post_stream(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *body = NULL;
  size_t body_len = 0;

  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    body = luaL_checklstring(L, 2, &body_len);
  }

  struct curl_slist *headers = parse_headers(L, 3);
  lua_Integer timeout_ms = luaL_optinteger(L, 5, 0);
  int background = lua_opt_background(L, 6);

  luaL_checktype(L, 4, LUA_TFUNCTION);
  lua_pushvalue(L, 4);
  int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  CURL *easy = curl_easy_init();
  if (!easy) {
    luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
    curl_slist_free_all(headers);
    return luaL_error(L, "failed to initialize curl easy handle");
  }

  StreamCtx *ctx = calloc(1, sizeof(StreamCtx));
  if (!ctx) {
    luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return luaL_error(L, "failed to allocate stream context");
  }
  ctx->easy = easy;
  ctx->L = L;
  ctx->callback_ref = callback_ref;
  ctx->headers = headers;
  ctx->background = background;
  ctx->response_headers.max_size = HTTP_MAX_HEADER_BYTES;

  configure_http_handle(easy, url);
  curl_easy_setopt(easy, CURLOPT_POST, 1L);
  if (timeout_ms > 0)
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  if (body) {
    ctx->body = malloc(body_len + 1);
    if (!ctx->body) {
      luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
      curl_slist_free_all(headers);
      curl_easy_cleanup(easy);
      stream_ctx_free(ctx);
      return luaL_error(L, "failed to allocate stream request body");
    }
    memcpy(ctx->body, body, body_len);
    ctx->body[body_len] = '\0';
    ctx->body_len = body_len;
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ctx->body);
  }
  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, stream_write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, ctx);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, &ctx->response_headers);

  if (stream_count >= stream_cap) {
    int new_cap = stream_cap ? stream_cap * 2 : 4;
    StreamCtx **new_streams = realloc(streams, new_cap * sizeof(StreamCtx *));
    if (!new_streams) {
      luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
      curl_slist_free_all(headers);
      curl_easy_cleanup(easy);
      stream_ctx_free(ctx);
      return luaL_error(L, "failed to allocate stream registry");
    }
    streams = new_streams;
    stream_cap = new_cap;
  }

  curl_multi_add_handle(multi_handle, easy);

  ctx->async_id = g_next_async_id;
  g_next_async_id = g_next_async_id == INT_MAX ? 1 : g_next_async_id + 1;
  streams[stream_count] = ctx;
  stream_count++;

  lua_pushinteger(L, ctx->async_id);
  return 1;
}

static int l_http_post(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *body = NULL;
  size_t body_len = 0;

  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    body = luaL_checklstring(L, 2, &body_len);
  }

  struct curl_slist *headers = parse_headers(L, 3);

  CURL *easy = curl_easy_init();
  configure_http_handle(easy, url);
  curl_easy_setopt(easy, CURLOPT_POST, 1L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, HTTP_DEFAULT_TIMEOUT_MS);

  if (body) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
  }

  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  RespBuf response = {0};
  response.max_size = HTTP_MAX_RESPONSE_BYTES;
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);

  g_sync_active = 1;
  curl_multi_add_handle(multi_handle, easy);

  int still_running = 1;
  while (still_running) {
    curl_multi_perform(multi_handle, &still_running);
    if (still_running)
      http_wait_frame();
  }

  http_poll(L);
  curl_multi_remove_handle(multi_handle, easy);
  g_sync_active = 0;

  long status = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  if (response.overflow) {
    lua_pushinteger(L, 0);
    lua_pushliteral(L, "Response too large");
  } else {
    lua_pushinteger(L, status);
    lua_pushstring(L, response.data ? response.data : "");
  }
  free(response.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return 2;
}

static int l_http_post_response(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *body = NULL;
  size_t body_len = 0;

  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    body = luaL_checklstring(L, 2, &body_len);
  }

  struct curl_slist *headers = parse_headers(L, 3);
  lua_Integer timeout_ms = luaL_optinteger(L, 4, 30000);

  CURL *easy = curl_easy_init();
  configure_http_handle(easy, url);
  curl_easy_setopt(easy, CURLOPT_POST, 1L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                   (long)(timeout_ms > 0 ? timeout_ms : HTTP_DEFAULT_TIMEOUT_MS));

  if (body) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
  }

  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  RespBuf response = {0};
  RespBuf response_headers = {0};
  response.max_size = HTTP_MAX_RESPONSE_BYTES;
  response_headers.max_size = HTTP_MAX_HEADER_BYTES;
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response_headers);

  g_sync_active = 1;
  curl_multi_add_handle(multi_handle, easy);

  int still_running = 1;
  while (still_running) {
    curl_multi_perform(multi_handle, &still_running);
    if (still_running)
      http_wait_frame();
  }

  http_poll(L);
  curl_multi_remove_handle(multi_handle, easy);
  g_sync_active = 0;

  long status = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

  lua_newtable(L);

  lua_pushinteger(L, response.overflow ? 0 : status);
  lua_setfield(L, -2, "status");

  lua_pushstring(L, response.overflow ? "Response too large" :
                                      (response.data ? response.data : ""));
  lua_setfield(L, -2, "body");

  push_headers_table(L, response_headers.data);
  lua_setfield(L, -2, "headers");

  free(response.data);
  free(response_headers.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return 1;
}

static int l_http_delete_response(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  struct curl_slist *headers = parse_headers(L, 2);
  lua_Integer timeout_ms = luaL_optinteger(L, 3, 30000);

  CURL *easy = curl_easy_init();
  if (!easy) {
    curl_slist_free_all(headers);
    return luaL_error(L, "failed to initialize curl easy handle");
  }
  configure_http_handle(easy, url);
  curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                   (long)(timeout_ms > 0 ? timeout_ms : HTTP_DEFAULT_TIMEOUT_MS));
  if (headers)
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

  RespBuf response = {0};
  RespBuf response_headers = {0};
  response.max_size = HTTP_MAX_RESPONSE_BYTES;
  response_headers.max_size = HTTP_MAX_HEADER_BYTES;
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response_headers);

  g_sync_active = 1;
  curl_multi_add_handle(multi_handle, easy);
  int still_running = 1;
  while (still_running) {
    curl_multi_perform(multi_handle, &still_running);
    if (still_running)
      http_wait_frame();
  }
  http_poll(L);
  curl_multi_remove_handle(multi_handle, easy);
  g_sync_active = 0;

  long status = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  lua_newtable(L);
  lua_pushinteger(L, response.overflow ? 0 : status);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, response.overflow ? "Response too large" :
                                      (response.data ? response.data : ""));
  lua_setfield(L, -2, "body");
  push_headers_table(L, response_headers.data);
  lua_setfield(L, -2, "headers");

  free(response.data);
  free(response_headers.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return 1;
}

static int l_http_post_response_async(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *body = NULL;
  size_t body_len = 0;

  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    body = luaL_checklstring(L, 2, &body_len);
  }

  struct curl_slist *headers = parse_headers(L, 3);
  lua_Integer timeout_ms = luaL_optinteger(L, 4, 30000);
  int background = lua_opt_background(L, 6);

  luaL_checktype(L, 5, LUA_TFUNCTION);
  lua_pushvalue(L, 5);
  int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  CURL *easy = curl_easy_init();
  if (!easy) {
    luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
    curl_slist_free_all(headers);
    return luaL_error(L, "failed to initialize curl easy handle");
  }

  StreamCtx *ctx = calloc(1, sizeof(StreamCtx));
  if (!ctx) {
    luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return luaL_error(L, "failed to allocate async response context");
  }
  ctx->easy = easy;
  ctx->L = L;
  ctx->callback_ref = callback_ref;
  ctx->headers = headers;
  ctx->response_mode = 1;
  ctx->background = background;
  ctx->response.max_size = HTTP_MAX_RESPONSE_BYTES;
  ctx->response_headers.max_size = HTTP_MAX_HEADER_BYTES;

  configure_http_handle(easy, url);
  curl_easy_setopt(easy, CURLOPT_POST, 1L);
  if (timeout_ms > 0)
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms);

  if (body) {
    ctx->body = malloc(body_len + 1);
    if (!ctx->body) {
      luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
      curl_slist_free_all(headers);
      curl_easy_cleanup(easy);
      stream_ctx_free(ctx);
      return luaL_error(L, "failed to allocate async response body");
    }
    memcpy(ctx->body, body, body_len);
    ctx->body[body_len] = '\0';
    ctx->body_len = body_len;
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ctx->body);
  }

  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, stream_write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, ctx);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, &ctx->response_headers);

  if (stream_count >= stream_cap) {
    int new_cap = stream_cap ? stream_cap * 2 : 4;
    StreamCtx **new_streams = realloc(streams, new_cap * sizeof(StreamCtx *));
    if (!new_streams) {
      luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
      curl_slist_free_all(headers);
      curl_easy_cleanup(easy);
      stream_ctx_free(ctx);
      return luaL_error(L, "failed to allocate stream registry");
    }
    streams = new_streams;
    stream_cap = new_cap;
  }

  curl_multi_add_handle(multi_handle, easy);

  ctx->async_id = g_next_async_id;
  g_next_async_id = g_next_async_id == INT_MAX ? 1 : g_next_async_id + 1;
  streams[stream_count] = ctx;
  stream_count++;

  lua_pushinteger(L, ctx->async_id);
  return 1;
}

static int l_http_get(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  struct curl_slist *headers = parse_headers(L, 2);

  CURL *easy = curl_easy_init();
  configure_http_handle(easy, url);
  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  RespBuf response = {0};
  response.max_size = HTTP_MAX_RESPONSE_BYTES;
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);

  g_sync_active = 1;
  curl_multi_add_handle(multi_handle, easy);

  int still_running = 1;
  while (still_running) {
    curl_multi_perform(multi_handle, &still_running);
    if (still_running)
      http_wait_frame();
  }

  http_poll(L);
  curl_multi_remove_handle(multi_handle, easy);
  g_sync_active = 0;

  long status = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  if (response.overflow) {
    lua_pushinteger(L, 0);
    lua_pushliteral(L, "Response too large");
  } else {
    lua_pushinteger(L, status);
    lua_pushstring(L, response.data ? response.data : "");
  }

  free(response.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return 2;
}

static int l_http_poll(lua_State *L) {
  lua_pushinteger(L, http_poll(L));
  return 1;
}

static int l_http_wait_frame(lua_State *L) {
  (void)L;
  http_wait_frame();
  return 0;
}

static int l_http_is_loading(lua_State *L) {
  lua_pushboolean(L, http_is_loading());
  return 1;
}

/*
 * Called from the main loop on each iteration.
 * Advances curl_multi, delivers Lua callbacks for new chunks, and on stream
 * completion invokes the callback with (nil, true) before releasing resources.
 */
int http_poll_limited(lua_State *L, int max_callbacks) {
  if (!multi_handle)
    return 0;

  int still_running;
  curl_multi_perform(multi_handle, &still_running);

  int had_events = 0;
  CURLMsg *msg;
  int msgs_in_queue;
  int callbacks = 0;

  while ((msg = curl_multi_info_read(multi_handle, &msgs_in_queue))) {
    if (msg->msg != CURLMSG_DONE)
      continue;

    StreamCtx *found = NULL;
    int found_idx = -1;
    for (int i = 0; i < stream_count; i++) {
      if (streams[i] && streams[i]->easy == msg->easy_handle) {
        found = streams[i];
        found_idx = i;
        break;
      }
    }

    if (!found)
      continue;

    int cb_ref = found->callback_ref;
    struct curl_slist *h = found->headers;
    CURL *e = found->easy;
    char *err_body = NULL;
    if (found->err_len > 0) {
      err_body = malloc(found->err_len + 1);
      memcpy(err_body, found->err_buf, found->err_len + 1);
    }

    long http_status = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_status);
    CURLcode curl_rc = msg->data.result;
    int response_mode = found->response_mode;

    curl_multi_remove_handle(multi_handle, e);
    streams[found_idx] = NULL;
    curl_slist_free_all(h);
    curl_easy_cleanup(e);

    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref);

    int nargs = 0;
    if (response_mode) {
      if (found->response.overflow) {
        lua_pushnil(L);
        lua_pushliteral(L, "Response too large");
        nargs = 2;
      } else if (curl_rc != CURLE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "Connection error: %s", curl_easy_strerror(curl_rc));
        nargs = 2;
      } else {
        lua_newtable(L);
        lua_pushinteger(L, http_status);
        lua_setfield(L, -2, "status");
        lua_pushstring(L, found->response.data ? found->response.data : "");
        lua_setfield(L, -2, "body");
        push_headers_table(L, found->response_headers.data);
        lua_setfield(L, -2, "headers");
        lua_pushnil(L);
        nargs = 2;
      }
    } else {
      lua_pushnil(L);
      lua_pushboolean(L, 1);

      nargs = 2;
      int has_error = 0;
      if (curl_rc != CURLE_OK) {
        nargs = 3;
        has_error = 1;
        lua_pushfstring(L, "Connection error: %s", curl_easy_strerror(curl_rc));
      } else if (http_status > 0 && (http_status < 200 || http_status >= 300)) {
        nargs = 3;
        has_error = 1;
        lua_pushfstring(L, "HTTP %d", (int)http_status);
      }

      if (has_error) {
        if (err_body)
          lua_pushstring(L, err_body);
        else
          lua_pushnil(L);
        push_headers_table(L, found->response_headers.data);
        nargs = 5;
      }
    }

    int msgh = lua_gettop(L) - nargs - 1;

    if (lua_pcall(L, nargs, 0, msgh) != LUA_OK) {
      if (g_headless)
        fprintf(stderr, "capstan stream error: %s\n", lua_tostring(L, -1));
      else
        popup_show_message("Stream Error", lua_tostring(L, -1), 1);
      lua_settop(L, lua_gettop(L) - 2);
    } else {
      lua_settop(L, lua_gettop(L) - 1);
    }

    free(err_body);
    stream_ctx_free(found);

    luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);

    had_events = 1;
    callbacks++;
    if (max_callbacks > 0 && callbacks >= max_callbacks)
      break;
  }

  int write_idx = 0;
  for (int i = 0; i < stream_count; i++) {
    if (streams[i] != NULL) {
      streams[write_idx++] = streams[i];
    }
  }
  stream_count = write_idx;

  return had_events;
}

int http_poll(lua_State *L) { return http_poll_limited(L, 0); }

static void compact_streams(void) {
  int write_idx = 0;
  for (int i = 0; i < stream_count; i++) {
    if (streams[i] != NULL)
      streams[write_idx++] = streams[i];
  }
  stream_count = write_idx;
}

static void cancel_stream_at(lua_State *L, int index) {
  StreamCtx *ctx = streams[index];
  curl_multi_remove_handle(multi_handle, ctx->easy);
  luaL_unref(L, LUA_REGISTRYINDEX, ctx->callback_ref);
  curl_slist_free_all(ctx->headers);
  curl_easy_cleanup(ctx->easy);
  stream_ctx_free(ctx);
  streams[index] = NULL;
}

static int l_http_cancel(lua_State *L) {
  lua_Integer requested_id = luaL_checkinteger(L, 1);
  if (requested_id < 1 || requested_id > INT_MAX) {
    lua_pushboolean(L, 0);
    return 1;
  }
  int async_id = (int)requested_id;
  for (int i = 0; i < stream_count; i++) {
    if (streams[i] && streams[i]->async_id == async_id) {
      cancel_stream_at(L, i);
      compact_streams();
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  lua_pushboolean(L, 0);
  return 1;
}

int http_cancel_streams(lua_State *L) {
  if (!multi_handle || stream_count == 0)
    return 0;

  int canceled = 0;
  for (int i = 0; i < stream_count; i++) {
    StreamCtx *ctx = streams[i];
    if (!ctx || ctx->background)
      continue;
    cancel_stream_at(L, i);
    canceled++;
  }

  compact_streams();
  return canceled;
}

void http_init(lua_State *L) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  multi_handle = curl_multi_init();

  lua_newtable(L);

  lua_pushcfunction(L, l_http_get);
  lua_setfield(L, -2, "get");

  lua_pushcfunction(L, l_http_post);
  lua_setfield(L, -2, "post");

  lua_pushcfunction(L, l_http_post_response);
  lua_setfield(L, -2, "post_response");

  lua_pushcfunction(L, l_http_delete_response);
  lua_setfield(L, -2, "delete_response");

  lua_pushcfunction(L, l_http_post_response_async);
  lua_setfield(L, -2, "post_response_async");

  lua_pushcfunction(L, l_http_post_stream);
  lua_setfield(L, -2, "post_stream");

  lua_pushcfunction(L, l_http_poll);
  lua_setfield(L, -2, "poll");

  lua_pushcfunction(L, l_http_cancel);
  lua_setfield(L, -2, "cancel");

  lua_pushcfunction(L, l_http_wait_frame);
  lua_setfield(L, -2, "wait_frame");

  lua_pushcfunction(L, l_http_is_loading);
  lua_setfield(L, -2, "is_loading");

  lua_setglobal(L, "http");
}

void http_cleanup(void) {
  if (multi_handle) {
    curl_multi_cleanup(multi_handle);
    multi_handle = NULL;
  }
  free(streams);
  streams = NULL;
  stream_count = 0;
  stream_cap = 0;
  g_next_async_id = 1;
  curl_global_cleanup();
}
