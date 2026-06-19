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

static MunitTest tests[] = {
    {"/get_follows_redirect", test_http_get_follows_redirect, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite http_redirect_suite = {"/http_redirect", tests, NULL, 1,
                                  MUNIT_SUITE_OPTION_NONE};
