#include "oauth_native.h"
#include "oauth_crypto.h"
#include <lauxlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LISTENER_MT "capstan.oauth.listener"
#define REQUEST_MAX 16384

typedef struct {
    int fd, client;
    unsigned short port;
    size_t used;
    double deadline, client_deadline;
    char request[REQUEST_MAX + 1];
} OAuthListener;

static int failure(lua_State *L, const char *message) {
    lua_pushnil(L); lua_pushstring(L, message); return 2;
}
static const char *string_arg(lua_State *L, int index, size_t *len) {
    return lua_type(L, index) == LUA_TSTRING ? lua_tolstring(L, index, len) : NULL;
}
static int l_random(lua_State *L) {
    unsigned char bytes[OAUTH_CRYPTO_MAX_BYTES];
    char encoded[OAUTH_CRYPTO_MAX_BASE64URL + 1];
    int valid = 0;
    lua_Integer n = lua_tointegerx(L, 1, &valid);
    if (lua_type(L, 1) != LUA_TNUMBER || !valid || n < 1 || n > OAUTH_CRYPTO_MAX_BYTES)
        return failure(L, "invalid random byte count");
    if (!oauth_crypto_random(bytes, (size_t)n) ||
        !oauth_crypto_base64url_encode(bytes, (size_t)n, encoded, sizeof(encoded)))
        return failure(L, "random generation failed");
    lua_pushstring(L, encoded); return 1;
}
static int l_pkce(lua_State *L) {
    size_t len = 0; char out[44];
    const char *s = string_arg(L, 1, &len);
    if (!s || !oauth_crypto_pkce_s256(s, len, out, sizeof(out)))
        return failure(L, "invalid PKCE verifier");
    lua_pushstring(L, out); return 1;
}
static int l_decode(lua_State *L) {
    size_t len = 0, decoded = 0; unsigned char out[OAUTH_CRYPTO_MAX_BYTES];
    const char *s = string_arg(L, 1, &len);
    if (!s || !oauth_crypto_base64url_decode(s, len, out, sizeof(out), &decoded))
        return failure(L, "invalid base64url encoding");
    lua_pushlstring(L, (const char *)out, decoded); return 1;
}
static int l_verify(lua_State *L) {
    size_t sizes[4] = {0}; const char *args[4];
    for (int i = 0; i < 4; i++) {
        args[i] = string_arg(L, i + 1, &sizes[i]);
        if (!args[i]) { lua_pushboolean(L, 0); return 1; }
    }
    lua_pushboolean(L, oauth_crypto_verify_rs256(args[0], sizes[0], args[1], sizes[1],
                                               args[2], sizes[2], args[3], sizes[3]));
    return 1;
}
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void close_client(OAuthListener *h) {
    if (h->client >= 0) close(h->client);
    h->client = -1; h->used = 0;
}
static void close_listener(OAuthListener *h) {
    close_client(h);
    if (h->fd >= 0) close(h->fd);
    h->fd = -1;
}
static int configure_socket(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return 0;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) return 0;
#endif
    return 1;
}
static int l_close(lua_State *L) {
    OAuthListener *h = luaL_testudata(L, 1, LISTENER_MT);
    if (!h) return failure(L, "invalid listener");
    close_listener(h); lua_pushboolean(L, 1); return 1;
}
static int l_listen(lua_State *L) {
    OAuthListener *h = lua_newuserdatauv(L, sizeof(*h), 0);
    memset(h, 0, sizeof(*h)); h->fd = h->client = -1;
    luaL_setmetatable(L, LISTENER_MT);
    double start = now();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(0x7f000001UL);
    h->fd = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t len = sizeof(addr);
    if (start < 0 || h->fd < 0 || !configure_socket(h->fd) ||
        bind(h->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockname(h->fd, (struct sockaddr *)&addr, &len) < 0 || listen(h->fd, 4) < 0) {
        close_listener(h); return failure(L, "loopback listener unavailable");
    }
    h->port = ntohs(addr.sin_port); h->deadline = start + 300;
    lua_pushfstring(L, "http://127.0.0.1:%d/auth/callback", (int)h->port);
    return 2;
}
/* Deliberately accept only the exact loopback authority and origin-form GET. */
static char *parse_request(OAuthListener *h) {
    char *s = h->request, *line = strstr(s, "\r\n");
    if (!line) return NULL;
    *line = 0;
    if (strncmp(s, "GET /auth/callback?", 19) != 0) return NULL;
    char *query = s + 19, *version = strchr(query, ' ');
    if (!version || (strcmp(version, " HTTP/1.1") && strcmp(version, " HTTP/1.0"))) return NULL;
    *version = 0;
    for (char *p = query; *p; p++) if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '#') return NULL;
    char authority[40]; snprintf(authority, sizeof(authority), "127.0.0.1:%u", h->port);
    int hosts = 0;
    for (s = line + 2; *s; s = line + 2) {
        line = strstr(s, "\r\n"); if (!line) return NULL;
        *line = 0; if (!*s) break;
        char *colon = strchr(s, ':'); if (!colon || colon == s) return NULL;
        for (char *p = s; p < colon; p++)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-')) return NULL;
        *colon = 0; char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        for (char *p = value; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return NULL;
        if (!strcasecmp(s, "Host")) { if (++hosts != 1 || strcmp(value, authority)) return NULL; }
        if (!strcasecmp(s, "Transfer-Encoding") ||
            (!strcasecmp(s, "Content-Length") && strcmp(value, "0"))) return NULL;
    }
    return hosts == 1 ? query : NULL;
}
static void respond(OAuthListener *h, int ok) {
    const char *response = ok ?
        "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 36\r\n\r\nYou may return to the application.\r\n" :
        "HTTP/1.1 400 Bad Request\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    /* Best effort only: never block the caller on a browser reading a response. */
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    (void)send(h->client, response, strlen(response), flags);
}
static int l_poll(lua_State *L) {
    OAuthListener *h = luaL_testudata(L, 1, LISTENER_MT);
    if (!h) return failure(L, "invalid listener");
    if (h->fd < 0) return failure(L, "listener closed");
    double t = now();
    if (t < 0 || t >= h->deadline) { close_listener(h); return failure(L, "listener expired"); }
    if (h->client >= 0 && t >= h->client_deadline) {
        close_client(h); return failure(L, "callback request timed out");
    }
    if (h->client < 0) {
        h->client = accept(h->fd, NULL, NULL);
        if (h->client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { lua_pushnil(L); return 1; }
            close_listener(h); return failure(L, "callback accept failed");
        }
        if (!configure_socket(h->client)) { close_client(h); return failure(L, "callback socket failed"); }
        h->client_deadline = t + 5;
    }
    ssize_t n = recv(h->client, h->request + h->used, REQUEST_MAX - h->used, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) { lua_pushnil(L); return 1; }
    if (n <= 0) { close_client(h); return failure(L, "callback connection closed"); }
    if (memchr(h->request + h->used, 0, (size_t)n)) {
        respond(h, 0); close_client(h); return failure(L, "invalid callback request");
    }
    h->used += (size_t)n; h->request[h->used] = 0;
    char *end = strstr(h->request, "\r\n\r\n");
    if (!end && h->used < REQUEST_MAX) { lua_pushnil(L); return 1; }
    char *query = end && (size_t)(end + 4 - h->request) == h->used ? parse_request(h) : NULL;
    respond(h, query != NULL);
    if (!query) { close_client(h); return failure(L, "invalid callback request"); }
    lua_pushstring(L, query); close_listener(h); return 1;
}
void oauth_native_init(lua_State *L) {
    const luaL_Reg methods[] = {{"poll", l_poll}, {"close", l_close}, {"__gc", l_close}, {NULL, NULL}};
    luaL_newmetatable(L, LISTENER_MT);
    luaL_setfuncs(L, methods, 0); lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "OAuth listener"); lua_setfield(L, -2, "__metatable"); lua_pop(L, 1);
    lua_getglobal(L, "capstan");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setglobal(L, "capstan"); }
    const luaL_Reg functions[] = {{"random_base64url", l_random}, {"pkce_s256", l_pkce},
        {"base64url_decode", l_decode}, {"verify_rs256", l_verify}, {"listen", l_listen}, {NULL, NULL}};
    luaL_newlib(L, functions); lua_setfield(L, -2, "oauth"); lua_pop(L, 1);
}
