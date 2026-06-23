#include "app_config.h"
#include "dyn_arr.h"
#include "permit.h"
#include "tui.h"
#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <lua.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern lua_State *L;

static PermEntries g_entries = {0};

const char *permit_config_dir(void) {
  static char path[512];
  if (app_config_dir(path, sizeof(path)) != 0)
    return NULL;
  return path;
}

PermState permit_check(const char *tool, const char *target) {
  for (int i = (int)g_entries.size - 1; i >= 0; i--) {
    PermEntry *e = g_entries.items[i];
    if (strcmp(e->tool, tool) == 0 && permit_pattern_match(e->pattern, target))
      return e->allow ? PERM_ALLOW : PERM_DENY;
  }

  if (strcmp(tool, "shell") == 0)
    return PERM_ASK;

  if (strcmp(tool, "file_read") == 0) {
    return permit_file_read_check(app_workdir(), target);
  }

  return PERM_ASK;
}

void permit_grant(const char *tool, const char *pattern, int allow) {
  for (size_t i = 0; i < g_entries.size; i++) {
    PermEntry *e = g_entries.items[i];
    if (strcmp(e->tool, tool) == 0 &&
        strcmp(e->pattern, pattern) == 0) {
      e->allow = allow;
      return;
    }
  }

  PermEntry *e = malloc(sizeof(PermEntry));
  e->tool = my_strdup(tool);
  e->pattern = my_strdup(pattern);
  e->allow = allow;
  da_append(&g_entries, e);
}

void permit_load(const char *path) {
  if (!L)
    return;

  if (luaL_dofile(L, path) != LUA_OK) {
    lua_pop(L, 1);
    return;
  }

  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  int len = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    const char *tool = NULL, *pattern = NULL;
    int allow = -1;

    lua_getfield(L, -1, "tool");
    tool = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "pattern");
    pattern = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "allow");
    if (!lua_isnil(L, -1))
      allow = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (tool && pattern && allow != -1)
      permit_grant(tool, pattern, allow);

    lua_pop(L, 1);
  }

  lua_pop(L, 1);
}

void permit_save(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return;

  fprintf(f, "return {\n");
  for (size_t i = 0; i < g_entries.size; i++) {
    PermEntry *e = g_entries.items[i];
    char tool[PERMIT_MAX_TARGET * 2];
    char pattern[PERMIT_MAX_TARGET * 2];
    if (!permit_lua_escape_string(e->tool, tool, sizeof(tool)) ||
        !permit_lua_escape_string(e->pattern, pattern, sizeof(pattern)))
      continue;
    fprintf(f, "  {tool = \"%s\", pattern = \"%s\", allow = %s},\n",
            tool, pattern, e->allow ? "true" : "false");
  }
  fprintf(f, "}\n");
  fclose(f);
}

static void permit_load_config_permissions(lua_State *L) {
  lua_getglobal(L, "capstan");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  lua_getfield(L, -1, "config");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return;
  }

  lua_getfield(L, -1, "permissions");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 3);
    return;
  }

  int len = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    lua_getfield(L, -1, "tool");
    const char *tool = lua_tostring(L, -1);
    lua_getfield(L, -2, "pattern");
    const char *pattern = lua_tostring(L, -1);
    lua_getfield(L, -3, "allow");
    int has_allow = !lua_isnil(L, -1);
    int allow = lua_toboolean(L, -1);

    if (tool && pattern && has_allow)
      permit_grant(tool, pattern, allow);

    lua_pop(L, 4);
  }

  lua_pop(L, 3);
}

static int l_permit_check(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *target = luaL_checkstring(L, 2);
  PermState s = permit_check(tool, target);
  switch (s) {
  case PERM_ALLOW:
    lua_pushstring(L, "allow");
    break;
  case PERM_DENY:
    lua_pushstring(L, "deny");
    break;
  default:
    lua_pushstring(L, "ask");
    break;
  }
  return 1;
}

static int l_permit_grant(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *pattern = luaL_checkstring(L, 2);
  int allow = lua_toboolean(L, 3);
  permit_grant(tool, pattern, allow);
  return 0;
}

static int l_permit_save(lua_State *L) {
  char path[512];
  if (app_state_ensure_dir() != 0 ||
      app_state_path(path, sizeof(path), "permissions.lua") != 0) {
    lua_pushboolean(L, 0);
    return 1;
  }
  permit_save(path);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_permit_load(lua_State *L) {
  char path[512];
  if (app_state_path(path, sizeof(path), "permissions.lua") != 0) {
    lua_pushboolean(L, 0);
    return 1;
  }
  permit_load(path);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_permit_prompt(lua_State *L) {
  const char *tool = luaL_checkstring(L, 1);
  const char *target = luaL_checkstring(L, 2);
  const char *result = tui_permit_prompt(tool, target);
  lua_pushstring(L, result);
  return 1;
}

void permit_init(lua_State *L) {
  char path[512];
  permit_load_config_permissions(L);
  if (app_state_path(path, sizeof(path), "permissions.lua") == 0)
    permit_load(path);

  lua_newtable(L);

  lua_pushcfunction(L, l_permit_check);
  lua_setfield(L, -2, "check");

  lua_pushcfunction(L, l_permit_grant);
  lua_setfield(L, -2, "grant");

  lua_pushcfunction(L, l_permit_save);
  lua_setfield(L, -2, "save");

  lua_pushcfunction(L, l_permit_load);
  lua_setfield(L, -2, "load");

  lua_pushcfunction(L, l_permit_prompt);
  lua_setfield(L, -2, "prompt");

  lua_setglobal(L, "permit");
}

static void sigalrm_handler(int sig) { (void)sig; }

static int l_tools_shell(lua_State *L) {
  const char *command = luaL_checkstring(L, 1);
  int timeout = PERMIT_DEFAULT_SHELL_TIMEOUT;
  if (lua_gettop(L) >= 2)
    timeout = (int)luaL_checkinteger(L, 2);
  if (timeout <= 0)
    timeout = PERMIT_DEFAULT_SHELL_TIMEOUT;

  int out_pipe[2], err_pipe[2];
  if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
    lua_newtable(L);
    lua_pushinteger(L, -1);
    lua_setfield(L, -2, "exit");
    lua_pushstring(L, "pipe() failed");
    lua_setfield(L, -2, "stdout");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "stderr");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "timed_out");
    return 1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    lua_newtable(L);
    lua_pushinteger(L, -1);
    lua_setfield(L, -2, "exit");
    lua_pushstring(L, "fork() failed");
    lua_setfield(L, -2, "stdout");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "stderr");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "timed_out");
    return 1;
  }

  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);

    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) {
      dup2(nullfd, STDIN_FILENO);
      close(nullfd);
    }

    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);

    chdir(app_workdir());
    alarm(timeout);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);

  struct sigaction sa_old, sa_new;
  sa_new.sa_handler = sigalrm_handler;
  sigemptyset(&sa_new.sa_mask);
  sa_new.sa_flags = 0;
  sigaction(SIGALRM, &sa_new, &sa_old);
  alarm(timeout);

  char *out_buf = malloc(PERMIT_MAX_STDOUT + 1);
  char *err_buf = malloc(PERMIT_MAX_STDERR + 1);
  size_t out_len = 0, err_len = 0;
  int timed_out = 0;

  out_buf[0] = '\0';
  err_buf[0] = '\0';

  while (1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(out_pipe[0], &rfds);
    FD_SET(err_pipe[0], &rfds);
    int maxfd = out_pipe[0] > err_pipe[0] ? out_pipe[0] : err_pipe[0];

    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
    int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (n < 0) {
      if (errno == EINTR) {
        timed_out = 1;
        kill(pid, SIGKILL);
        break;
      }
      break;
    }
    if (n == 0)
      continue;

    if (FD_ISSET(out_pipe[0], &rfds)) {
      char tmp[4096];
      ssize_t r = read(out_pipe[0], tmp, sizeof(tmp));
      if (r <= 0)
        FD_CLR(out_pipe[0], &rfds);
      else if (out_len + r < PERMIT_MAX_STDOUT) {
        memcpy(out_buf + out_len, tmp, r);
        out_len += r;
        out_buf[out_len] = '\0';
      }
    }

    if (FD_ISSET(err_pipe[0], &rfds)) {
      char tmp[4096];
      ssize_t r = read(err_pipe[0], tmp, sizeof(tmp));
      if (r <= 0)
        FD_CLR(err_pipe[0], &rfds);
      else if (err_len + r < PERMIT_MAX_STDERR) {
        memcpy(err_buf + err_len, tmp, r);
        err_len += r;
        err_buf[err_len] = '\0';
      }
    }

    if (!FD_ISSET(out_pipe[0], &rfds) && !FD_ISSET(err_pipe[0], &rfds))
      break;
  }

  alarm(0);
  sigaction(SIGALRM, &sa_old, NULL);

  int status = 0;
  int exit_code = -1;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    exit_code = 128 + WTERMSIG(status);

  close(out_pipe[0]);
  close(err_pipe[0]);

  lua_newtable(L);
  lua_pushinteger(L, exit_code);
  lua_setfield(L, -2, "exit");
  lua_pushstring(L, out_buf);
  lua_setfield(L, -2, "stdout");
  lua_pushstring(L, err_buf);
  lua_setfield(L, -2, "stderr");
  lua_pushboolean(L, timed_out);
  lua_setfield(L, -2, "timed_out");

  free(out_buf);
  free(err_buf);

  return 1;
}

void tools_init(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_tools_shell);
  lua_setfield(L, -2, "shell");
  lua_setglobal(L, "tools");
}
