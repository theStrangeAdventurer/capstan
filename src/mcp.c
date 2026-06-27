#include "tui.h"
#include <errno.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <lua.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * MCP subprocess manager — bidirectional stdio for JSON-RPC over NDJSON.
 *
 * Each handle is a long-lived child process with:
 *   - stdin pipe (write side)
 *   - stdout pipe (read side)
 *   - stderr piped to /dev/null
 *
 * recv() reads one line at a time with a timeout, pumping the UI
 * via tui_pump_blocking() so the spinner stays alive.
 */

#define MCP_MAX_PROCS 32

typedef struct {
  pid_t pid;
  int stdin_fd;   /* write end */
  int stdout_fd;  /* read end */
  int alive;
} McpProc;

static McpProc g_procs[MCP_MAX_PROCS];
static int g_proc_count = 0;

static int find_free_slot(void) {
  for (int i = 0; i < MCP_MAX_PROCS; i++) {
    if (!g_procs[i].alive)
      return i;
  }
  return -1;
}

static int l_mcp_spawn(lua_State *L) {
  const char *command = luaL_checkstring(L, 1);
  /* args: array of strings (2nd arg, optional) */
  /* env: table of key=string (3rd arg, optional) */

  /* Build argv array */
  lua_Integer nargs = 0;
  if (lua_istable(L, 2))
    nargs = (lua_Integer)lua_rawlen(L, 2);

  /* argv[0] = command, argv[1..n] = args, argv[n+1] = NULL */
  int argv_count = (int)nargs + 2;
  char **argv = calloc((size_t)argv_count, sizeof(char *));
  if (!argv)
    return luaL_error(L, "mcp.spawn: out of memory");

  argv[0] = strdup(command);
  if (!argv[0]) {
    free(argv);
    return luaL_error(L, "mcp.spawn: out of memory");
  }

  for (lua_Integer i = 1; i <= nargs; i++) {
    lua_rawgeti(L, 2, (int)i);
    const char *s = lua_tostring(L, -1);
    argv[(int)i] = s ? strdup(s) : strdup("");
    lua_pop(L, 1);
  }
  argv[(int)nargs + 1] = NULL;

  /* Build env array if provided with at least one string-keyed override */
  char **envp = NULL;
  if (lua_istable(L, 3)) {
    /* Count overrides from table (string keys only) */
    int str_keys = 0;
    lua_pushnil(L);
    while (lua_next(L, 3) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING)
        str_keys++;
      lua_pop(L, 1);
    }

    /* Only build a custom envp if there are actual overrides.
     * Otherwise leave envp=NULL so the child uses execvp() (PATH-aware).
     * execve() does NOT search PATH and would fail for commands like "npx". */
    if (str_keys > 0) {
      /* Count existing environ entries */
      extern char **environ;
      int env_count = 0;
      for (char **e = environ; *e; e++)
        env_count++;

      envp = calloc((size_t)(env_count + str_keys + 1), sizeof(char *));
      if (!envp) {
        for (int i = 0; i < argv_count - 1; i++)
          free(argv[i]);
        free(argv);
        return luaL_error(L, "mcp.spawn: out of memory");
      }

      /* Copy current environ */
      int idx = 0;
      for (char **e = environ; *e; e++) {
        envp[idx++] = strdup(*e);
      }

      /* Apply overrides: format "KEY=VALUE" */
      lua_pushnil(L);
      while (lua_next(L, 3) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          const char *key = lua_tostring(L, -2);
          const char *val = lua_tostring(L, -1);
          if (key && val) {
            size_t len = strlen(key) + 1 + strlen(val) + 1;
            char *entry = malloc(len);
            if (entry)
              snprintf(entry, len, "%s=%s", key, val);
            envp[idx++] = entry;
          }
        }
        lua_pop(L, 1);
      }
      envp[idx] = NULL;
    }
  }

  int slot = find_free_slot();
  if (slot < 0) {
    for (int i = 0; i < argv_count - 1; i++)
      free(argv[i]);
    free(argv);
    if (envp) {
      for (int i = 0; envp[i]; i++)
        free(envp[i]);
      free(envp);
    }
    return luaL_error(L, "mcp.spawn: too many processes (max %d)", MCP_MAX_PROCS);
  }

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) < 0) {
    for (int i = 0; i < argv_count - 1; i++)
      free(argv[i]);
    free(argv);
    if (envp) {
      for (int i = 0; envp[i]; i++)
        free(envp[i]);
      free(envp);
    }
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }
  if (pipe(out_pipe) < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    for (int i = 0; i < argv_count - 1; i++)
      free(argv[i]);
    free(argv);
    if (envp) {
      for (int i = 0; envp[i]; i++)
        free(envp[i]);
      free(envp);
    }
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    for (int i = 0; i < argv_count - 1; i++)
      free(argv[i]);
    free(argv);
    if (envp) {
      for (int i = 0; envp[i]; i++)
        free(envp[i]);
      free(envp);
    }
    return luaL_error(L, "mcp.spawn: fork() failed: %s", strerror(errno));
  }

  if (pid == 0) {
    /* Child */
    /* stdin = in_pipe[0] (read end) */
    dup2(in_pipe[0], STDIN_FILENO);
    /* stdout = out_pipe[1] (write end) */
    dup2(out_pipe[1], STDOUT_FILENO);
    /* stderr → /dev/null */
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);

    if (envp)
      execve(command, argv, envp);
    else
      execvp(command, argv);

    /* exec failed */
    _exit(127);
  }

  /* Parent */
  close(in_pipe[0]);
  close(out_pipe[1]);

  /* Set stdout read end to non-blocking for select() */
  int flags = fcntl(out_pipe[0], F_GETFL, 0);
  fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

  g_procs[slot].pid = pid;
  g_procs[slot].stdin_fd = in_pipe[1];
  g_procs[slot].stdout_fd = out_pipe[0];
  g_procs[slot].alive = 1;
  g_proc_count++;

  /* Cleanup argv/envp (child has its own copies after fork) */
  for (int i = 0; i < argv_count - 1; i++)
    free(argv[i]);
  free(argv);
  if (envp) {
    for (int i = 0; envp[i]; i++)
      free(envp[i]);
    free(envp);
  }

  lua_pushinteger(L, slot);
  return 1;
}

static int l_mcp_send(lua_State *L) {
  int handle = (int)luaL_checkinteger(L, 1);
  size_t msg_len;
  const char *msg = luaL_checklstring(L, 2, &msg_len);

  if (handle < 0 || handle >= MCP_MAX_PROCS || !g_procs[handle].alive) {
    lua_pushnil(L);
    lua_pushstring(L, "mcp.send: invalid or dead handle");
    return 2;
  }

  /* Write message + newline */
  ssize_t w1 = write(g_procs[handle].stdin_fd, msg, msg_len);
  ssize_t w2 = write(g_procs[handle].stdin_fd, "\n", 1);

  if (w1 < 0 || w2 < 0) {
    lua_pushnil(L);
    lua_pushstring(L, "mcp.send: write failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  return 1;
}

static int l_mcp_recv(lua_State *L) {
  int handle = (int)luaL_checkinteger(L, 1);
  lua_Integer timeout_ms = luaL_optinteger(L, 2, 30000);

  if (handle < 0 || handle >= MCP_MAX_PROCS || !g_procs[handle].alive) {
    lua_pushnil(L);
    lua_pushstring(L, "mcp.recv: invalid or dead handle");
    return 2;
  }

  int fd = g_procs[handle].stdout_fd;

  /* Use a dynamic buffer for the line */
  char *buf = NULL;
  size_t buf_len = 0;
  size_t buf_cap = 0;

  struct timeval start;
  gettimeofday(&start, NULL);
  long long deadline_ms =
      (long long)start.tv_sec * 1000 + start.tv_usec / 1000 + timeout_ms;

  for (;;) {
    /* Check timeout */
    struct timeval now;
    gettimeofday(&now, NULL);
    long long now_ms = (long long)now.tv_sec * 1000 + now.tv_usec / 1000;
    if (now_ms >= deadline_ms) {
      free(buf);
      lua_pushnil(L);
      lua_pushstring(L, "timeout");
      return 2;
    }

    /* Check if process died */
    int status;
    pid_t r = waitpid(g_procs[handle].pid, &status, WNOHANG);
    if (r == g_procs[handle].pid) {
      g_procs[handle].alive = 0;
      g_proc_count--;
      free(buf);
      lua_pushnil(L);
      lua_pushstring(L, "process exited");
      return 2;
    }

    /* select with 100ms timeout, pump UI */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
    int n = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      free(buf);
      lua_pushnil(L);
      lua_pushstring(L, "select() failed");
      return 2;
    }
    if (n == 0) {
      /* No data yet — pump UI and continue */
      tui_pump_blocking();
      continue;
    }

    /* Read available data byte-by-byte until we hit \n */
    char ch;
    ssize_t bytes_read = 0;
    while ((bytes_read = read(fd, &ch, 1)) == 1) {
      if (ch == '\n') {
        /* Complete line */
        if (buf_len == 0) {
          /* Empty line — skip */
          continue;
        }
        /* Return the line without the newline */
        if (!buf) {
          lua_pushliteral(L, "");
          return 1;
        }
        lua_pushlstring(L, buf, buf_len);
        free(buf);
        return 1;
      }

      /* Append char to buffer */
      if (buf_len + 1 >= buf_cap) {
        buf_cap = buf_cap ? buf_cap * 2 : 256;
        char *new_buf = realloc(buf, buf_cap);
        if (!new_buf) {
          free(buf);
          lua_pushnil(L);
          lua_pushstring(L, "out of memory");
          return 2;
        }
        buf = new_buf;
      }
      buf[buf_len++] = ch;
    }

    if (bytes_read < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Non-blocking read returned — continue loop */
        if (buf_len > 0) {
          /* We have partial data, keep trying */
          continue;
        }
        tui_pump_blocking();
        continue;
      }
      /* Real error */
      free(buf);
      lua_pushnil(L);
      lua_pushstring(L, "read() failed");
      return 2;
    }

    if (bytes_read == 0) {
      /* EOF — process closed stdout */
      if (buf_len > 0) {
        /* Return remaining data as last line */
        lua_pushlstring(L, buf, buf_len);
        free(buf);
        return 1;
      }
      free(buf);
      g_procs[handle].alive = 0;
      g_proc_count--;
      lua_pushnil(L);
      lua_pushstring(L, "process closed stdout");
      return 2;
    }
  }
}

static int l_mcp_alive(lua_State *L) {
  int handle = (int)luaL_checkinteger(L, 1);
  if (handle < 0 || handle >= MCP_MAX_PROCS) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (!g_procs[handle].alive) {
    lua_pushboolean(L, 0);
    return 1;
  }
  /* Double-check with waitpid */
  int status;
  pid_t r = waitpid(g_procs[handle].pid, &status, WNOHANG);
  if (r == g_procs[handle].pid) {
    g_procs[handle].alive = 0;
    g_proc_count--;
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int l_mcp_kill(lua_State *L) {
  int handle = (int)luaL_optinteger(L, 1, -1);
  if (handle < 0 || handle >= MCP_MAX_PROCS || !g_procs[handle].alive) {
    lua_pushboolean(L, 0);
    return 1;
  }

  /* Close stdin */
  if (g_procs[handle].stdin_fd >= 0) {
    close(g_procs[handle].stdin_fd);
    g_procs[handle].stdin_fd = -1;
  }

  /* SIGTERM */
  kill(g_procs[handle].pid, SIGTERM);

  /* Wait up to 2 seconds */
  for (int i = 0; i < 20; i++) {
    int status;
    pid_t r = waitpid(g_procs[handle].pid, &status, WNOHANG);
    if (r == g_procs[handle].pid) {
      break;
    }
    usleep(100000); /* 100ms */
  }

  /* SIGKILL if still alive */
  int status;
  pid_t r = waitpid(g_procs[handle].pid, &status, WNOHANG);
  if (r != g_procs[handle].pid) {
    kill(g_procs[handle].pid, SIGKILL);
    waitpid(g_procs[handle].pid, &status, 0);
  }

  /* Close stdout */
  if (g_procs[handle].stdout_fd >= 0) {
    close(g_procs[handle].stdout_fd);
    g_procs[handle].stdout_fd = -1;
  }

  g_procs[handle].alive = 0;
  g_proc_count--;

  lua_pushboolean(L, 1);
  return 1;
}

/* Kill all alive procs — called at shutdown */
void mcp_cleanup(void) {
  for (int i = 0; i < MCP_MAX_PROCS; i++) {
    if (g_procs[i].alive) {
      if (g_procs[i].stdin_fd >= 0) {
        close(g_procs[i].stdin_fd);
        g_procs[i].stdin_fd = -1;
      }
      kill(g_procs[i].pid, SIGTERM);
      /* Brief wait */
      for (int j = 0; j < 10; j++) {
        int status;
        if (waitpid(g_procs[i].pid, &status, WNOHANG) == g_procs[i].pid)
          break;
        usleep(50000);
      }
      int status;
      if (waitpid(g_procs[i].pid, &status, WNOHANG) != g_procs[i].pid) {
        kill(g_procs[i].pid, SIGKILL);
        waitpid(g_procs[i].pid, &status, 0);
      }
      if (g_procs[i].stdout_fd >= 0) {
        close(g_procs[i].stdout_fd);
        g_procs[i].stdout_fd = -1;
      }
      g_procs[i].alive = 0;
    }
  }
  g_proc_count = 0;
}

void mcp_init(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, l_mcp_spawn);
  lua_setfield(L, -2, "spawn");

  lua_pushcfunction(L, l_mcp_send);
  lua_setfield(L, -2, "send");

  lua_pushcfunction(L, l_mcp_recv);
  lua_setfield(L, -2, "recv");

  lua_pushcfunction(L, l_mcp_alive);
  lua_setfield(L, -2, "alive");

  lua_pushcfunction(L, l_mcp_kill);
  lua_setfield(L, -2, "kill");

  lua_setglobal(L, "mcp");
}
