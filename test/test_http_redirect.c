#include "http.h"
#include "munit.h"
#include <arpa/inet.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int stream_done_calls;
static int stream_last_argc;
static int stream_last_is_done;
static int stream_last_has_err;
static int stream_last_has_body;
static int stream_seen_data;

void render_all(void) {}

int napms(int ms) {
  usleep((useconds_t)ms * 1000);
  return 0;
}

void popup_show_message(const char *title, const char *text, int is_error) {
  (void)title;
  (void)text;
  (void)is_error;
}

static void read_request(int fd) {
  char buf[1024];
  (void)read(fd, buf, sizeof(buf));
}

static void write_all(int fd, const char *s) {
  size_t len = strlen(s);
  while (len > 0) {
    ssize_t n = write(fd, s, len);
    if (n <= 0)
      return;
    s += n;
    len -= (size_t)n;
  }
}

static int accept_one(int server_fd) {
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  return accept(server_fd, (struct sockaddr *)&addr, &addr_len);
}

static pid_t start_redirect_server(int *port_out) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    return -1;

  int yes = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = htons(0);
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(server_fd);
    return -1;
  }
  if (listen(server_fd, 2) != 0) {
    close(server_fd);
    return -1;
  }

  socklen_t addr_len = sizeof(addr);
  if (getsockname(server_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(server_fd);
    return -1;
  }
  *port_out = ntohs(addr.sin_port);

  pid_t pid = fork();
  if (pid < 0) {
    close(server_fd);
    return -1;
  }
  if (pid == 0) {
    char redirect[512];
    snprintf(redirect, sizeof(redirect),
             "HTTP/1.1 302 Found\r\n"
             "Location: http://127.0.0.1:%d/final\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n",
             *port_out);

    int first = accept_one(server_fd);
    if (first >= 0) {
      read_request(first);
      write_all(first, redirect);
      close(first);
    }

    int second = accept_one(server_fd);
    if (second >= 0) {
      read_request(second);
      write_all(second,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 10\r\n"
                "Connection: close\r\n"
                "\r\n"
                "redirected");
      close(second);
    }

    close(server_fd);
    _exit(0);
  }

  close(server_fd);
  return pid;
}

static pid_t start_stream_server(int *port_out) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    return -1;

  int yes = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = htons(0);
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(server_fd);
    return -1;
  }
  if (listen(server_fd, 1) != 0) {
    close(server_fd);
    return -1;
  }

  socklen_t addr_len = sizeof(addr);
  if (getsockname(server_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(server_fd);
    return -1;
  }
  *port_out = ntohs(addr.sin_port);

  pid_t pid = fork();
  if (pid < 0) {
    close(server_fd);
    return -1;
  }
  if (pid == 0) {
    int client = accept_one(server_fd);
    if (client >= 0) {
      read_request(client);
      write_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n"
                "\r\n"
                "data: hello\n\n");
      close(client);
    }
    close(server_fd);
    _exit(0);
  }

  close(server_fd);
  return pid;
}

static pid_t start_post_response_server(int *port_out) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    return -1;

  int yes = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = htons(0);
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(server_fd);
    return -1;
  }
  if (listen(server_fd, 1) != 0) {
    close(server_fd);
    return -1;
  }

  socklen_t addr_len = sizeof(addr);
  if (getsockname(server_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(server_fd);
    return -1;
  }
  *port_out = ntohs(addr.sin_port);

  pid_t pid = fork();
  if (pid < 0) {
    close(server_fd);
    return -1;
  }
  if (pid == 0) {
    int client = accept_one(server_fd);
    if (client >= 0) {
      read_request(client);
      write_all(client,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Mcp-Session-Id: session-123\r\n"
                "Content-Length: 11\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"ok\":true}");
      close(client);
    }
    close(server_fd);
    _exit(0);
  }

  close(server_fd);
  return pid;
}

static int stream_capture_callback(lua_State *L) {
  int argc = lua_gettop(L);
  if (argc >= 1 && lua_isstring(L, 1))
    stream_seen_data = 1;
  if (argc >= 2 && lua_toboolean(L, 2)) {
    stream_done_calls++;
    stream_last_argc = argc;
    stream_last_is_done = 1;
    stream_last_has_err = argc >= 3 && !lua_isnil(L, 3);
    stream_last_has_body = argc >= 4 && !lua_isnil(L, 4);
  }
  return 0;
}

static MunitResult test_http_get_follows_redirect(const MunitParameter params[],
                                                  void *data) {
  (void)params;
  (void)data;

  int port = 0;
  pid_t server_pid = start_redirect_server(&port);
  if (server_pid < 0)
    return MUNIT_SKIP;

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  http_init(L);

  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", port);

  lua_getglobal(L, "http");
  lua_getfield(L, -1, "get");
  lua_pushstring(L, url);
  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);

  munit_assert_int((int)lua_tointeger(L, -2), ==, 200);
  munit_assert_string_equal(lua_tostring(L, -1), "redirected");

  lua_pop(L, 3);
  http_cleanup();
  lua_close(L);

  int status = 0;
  waitpid(server_pid, &status, 0);
  munit_assert_true(WIFEXITED(status));
  munit_assert_int(WEXITSTATUS(status), ==, 0);

  return MUNIT_OK;
}

static MunitResult test_post_stream_success_done_has_no_error_body(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  int port = 0;
  pid_t server_pid = start_stream_server(&port);
  if (server_pid < 0)
    return MUNIT_SKIP;

  stream_done_calls = 0;
  stream_last_argc = 0;
  stream_last_is_done = 0;
  stream_last_has_err = 0;
  stream_last_has_body = 0;
  stream_seen_data = 0;

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  http_init(L);

  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/stream", port);

  lua_getglobal(L, "http");
  lua_getfield(L, -1, "post_stream");
  lua_pushstring(L, url);
  lua_pushstring(L, "{}");
  lua_newtable(L);
  lua_pushcfunction(L, stream_capture_callback);
  int rc = lua_pcall(L, 4, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT, 0);

  for (int i = 0; i < 100 && stream_done_calls == 0; i++) {
    http_poll(L);
    usleep(10000);
  }

  munit_assert_int(stream_seen_data, ==, 1);
  munit_assert_int(stream_done_calls, ==, 1);
  munit_assert_int(stream_last_is_done, ==, 1);
  munit_assert_int(stream_last_argc, ==, 2);
  munit_assert_int(stream_last_has_err, ==, 0);
  munit_assert_int(stream_last_has_body, ==, 0);

  http_cleanup();
  lua_close(L);

  int status = 0;
  waitpid(server_pid, &status, 0);
  munit_assert_true(WIFEXITED(status));
  munit_assert_int(WEXITSTATUS(status), ==, 0);

  return MUNIT_OK;
}

static MunitResult test_post_response_returns_headers(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  int port = 0;
  pid_t server_pid = start_post_response_server(&port);
  if (server_pid < 0)
    return MUNIT_SKIP;

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  http_init(L);

  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/rpc", port);

  lua_getglobal(L, "http");
  lua_getfield(L, -1, "post_response");
  lua_pushstring(L, url);
  lua_pushstring(L, "{}");
  lua_newtable(L);
  lua_pushinteger(L, 1000);
  int rc = lua_pcall(L, 4, 1, 0);
  munit_assert_int(rc, ==, LUA_OK);

  lua_getfield(L, -1, "status");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 200);
  lua_pop(L, 1);

  lua_getfield(L, -1, "body");
  munit_assert_string_equal(lua_tostring(L, -1), "{\"ok\":true}");
  lua_pop(L, 1);

  lua_getfield(L, -1, "headers");
  lua_getfield(L, -1, "mcp-session-id");
  munit_assert_string_equal(lua_tostring(L, -1), "session-123");
  lua_pop(L, 2);

  lua_pop(L, 2);
  http_cleanup();
  lua_close(L);

  int status = 0;
  waitpid(server_pid, &status, 0);
  munit_assert_true(WIFEXITED(status));
  munit_assert_int(WEXITSTATUS(status), ==, 0);

  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/get_follows_redirect", test_http_get_follows_redirect, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/post_stream_success_done_has_no_error_body",
     test_post_stream_success_done_has_no_error_body, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/post_response_returns_headers", test_post_response_returns_headers, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite http_redirect_suite = {"/http_redirect", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
