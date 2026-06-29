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
  char *read_buf;
  size_t read_len;
  size_t read_cap;
} McpProc;

static McpProc g_procs[MCP_MAX_PROCS];
static int g_proc_count = 0;

static void free_string_array(char **items) {
  if (!items)
    return;
  for (int i = 0; items[i]; i++)
    free(items[i]);
  free(items);
}

static void close_proc_slot(int handle) {
  if (handle < 0 || handle >= MCP_MAX_PROCS)
    return;
  if (g_procs[handle].stdin_fd >= 0) {
    close(g_procs[handle].stdin_fd);
    g_procs[handle].stdin_fd = -1;
  }
  if (g_procs[handle].stdout_fd >= 0) {
    close(g_procs[handle].stdout_fd);
    g_procs[handle].stdout_fd = -1;
  }
  if (g_procs[handle].alive) {
    g_procs[handle].alive = 0;
    if (g_proc_count > 0)
      g_proc_count--;
  }
  free(g_procs[handle].read_buf);
  g_procs[handle].read_buf = NULL;
  g_procs[handle].read_len = 0;
  g_procs[handle].read_cap = 0;
  g_procs[handle].pid = 0;
}

static int write_all(int fd, const char *data, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(fd, data + written, len - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0) {
      errno = EPIPE;
      return -1;
    }
    written += (size_t)n;
  }
  return 0;
}

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

  argv[0] = my_strdup(command);
  if (!argv[0]) {
    free(argv);
    return luaL_error(L, "mcp.spawn: out of memory");
  }

  for (lua_Integer i = 1; i <= nargs; i++) {
    lua_rawgeti(L, 2, (int)i);
    const char *s = lua_tostring(L, -1);
    argv[(int)i] = my_strdup(s ? s : "");
    lua_pop(L, 1);
    if (!argv[(int)i]) {
      free_string_array(argv);
      return luaL_error(L, "mcp.spawn: out of memory");
    }
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
        if (!envp[idx - 1]) {
          free_string_array(argv);
          free_string_array(envp);
          return luaL_error(L, "mcp.spawn: out of memory");
        }
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
            if (!entry) {
              lua_pop(L, 1);
              free_string_array(argv);
              free_string_array(envp);
              return luaL_error(L, "mcp.spawn: out of memory");
            }
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
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: too many processes (max %d)", MCP_MAX_PROCS);
  }

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) < 0) {
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }
  if (pipe(out_pipe) < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }

  int exec_pipe[2];
  if (pipe(exec_pipe) < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }
  int exec_flags = fcntl(exec_pipe[1], F_GETFD, 0);
  if (exec_flags >= 0)
    fcntl(exec_pipe[1], F_SETFD, exec_flags | FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(exec_pipe[0]);
    close(exec_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
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
    close(exec_pipe[0]);

    if (envp)
      execve(command, argv, envp);
    else
      execvp(command, argv);

    /* exec failed */
    int err = errno;
    (void)write(exec_pipe[1], &err, sizeof(err));
    _exit(127);
  }

  /* Parent */
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(exec_pipe[1]);

  int exec_err = 0;
  fd_set efds;
  FD_ZERO(&efds);
  FD_SET(exec_pipe[0], &efds);
  struct timeval exec_tv = {.tv_sec = 0, .tv_usec = 50000};
  int exec_ready = select(exec_pipe[0] + 1, &efds, NULL, NULL, &exec_tv);
  if (exec_ready > 0 && FD_ISSET(exec_pipe[0], &efds)) {
    ssize_t n = read(exec_pipe[0], &exec_err, sizeof(exec_err));
    if (n == (ssize_t)sizeof(exec_err) && exec_err != 0) {
      close(exec_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      int status;
      waitpid(pid, &status, 0);
      free_string_array(argv);
      free_string_array(envp);
      return luaL_error(L, "mcp.spawn: exec failed: %s", strerror(exec_err));
    }
  }
  close(exec_pipe[0]);

  int child_status;
  pid_t child_done = waitpid(pid, &child_status, WNOHANG);
  if (child_done == pid) {
    close(in_pipe[1]);
    close(out_pipe[0]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: process exited during startup");
  }

  /* Set stdout read end to non-blocking for select() */
  int flags = fcntl(out_pipe[0], F_GETFL, 0);
  fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

  g_procs[slot].pid = pid;
  g_procs[slot].stdin_fd = in_pipe[1];
  g_procs[slot].stdout_fd = out_pipe[0];
  g_procs[slot].alive = 1;
  g_procs[slot].read_buf = NULL;
  g_procs[slot].read_len = 0;
  g_procs[slot].read_cap = 0;
  g_proc_count++;

  /* Cleanup argv/envp (child has its own copies after fork) */
  free_string_array(argv);
  free_string_array(envp);

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

  if (write_all(g_procs[handle].stdin_fd, msg, msg_len) != 0 ||
      write_all(g_procs[handle].stdin_fd, "\n", 1) != 0) {
    lua_pushnil(L);
    lua_pushfstring(L, "mcp.send: write failed: %s", strerror(errno));
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
      close_proc_slot(handle);
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
      close_proc_slot(handle);
      lua_pushnil(L);
      lua_pushstring(L, "process closed stdout");
      return 2;
    }
  }
}

static int mcp_append_read_char(McpProc *proc, char ch) {
  if (proc->read_len + 1 >= proc->read_cap) {
    size_t new_cap = proc->read_cap ? proc->read_cap * 2 : 256;
    char *new_buf = realloc(proc->read_buf, new_cap);
    if (!new_buf)
      return -1;
    proc->read_buf = new_buf;
    proc->read_cap = new_cap;
  }
  proc->read_buf[proc->read_len++] = ch;
  return 0;
}

static int l_mcp_recv_nowait(lua_State *L) {
  int handle = (int)luaL_checkinteger(L, 1);

  if (handle < 0 || handle >= MCP_MAX_PROCS || !g_procs[handle].alive) {
    lua_pushnil(L);
    lua_pushstring(L, "mcp.recv_nowait: invalid or dead handle");
    return 2;
  }

  McpProc *proc = &g_procs[handle];

  int status;
  pid_t r = waitpid(proc->pid, &status, WNOHANG);
  if (r == proc->pid) {
    close_proc_slot(handle);
    lua_pushnil(L);
    lua_pushstring(L, "process exited");
    return 2;
  }

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(proc->stdout_fd, &rfds);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
  int n = select(proc->stdout_fd + 1, &rfds, NULL, NULL, &tv);
  if (n < 0) {
    if (errno == EINTR) {
      lua_pushnil(L);
      lua_pushliteral(L, "again");
      return 2;
    }
    lua_pushnil(L);
    lua_pushstring(L, "select() failed");
    return 2;
  }
  if (n == 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "again");
    return 2;
  }

  char ch;
  ssize_t bytes_read;
  while ((bytes_read = read(proc->stdout_fd, &ch, 1)) == 1) {
    if (ch == '\n') {
      if (proc->read_len == 0)
        continue;
      lua_pushlstring(L, proc->read_buf, proc->read_len);
      proc->read_len = 0;
      return 1;
    }
    if (mcp_append_read_char(proc, ch) != 0) {
      lua_pushnil(L);
      lua_pushstring(L, "out of memory");
      return 2;
    }
  }

  if (bytes_read < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      lua_pushnil(L);
      lua_pushliteral(L, "again");
      return 2;
    }
    lua_pushnil(L);
    lua_pushstring(L, "read() failed");
    return 2;
  }

  if (proc->read_len > 0) {
    lua_pushlstring(L, proc->read_buf, proc->read_len);
    proc->read_len = 0;
    return 1;
  }

  close_proc_slot(handle);
  lua_pushnil(L);
  lua_pushstring(L, "process closed stdout");
  return 2;
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
    close_proc_slot(handle);
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
    tui_pump_blocking();
  }

  /* SIGKILL if still alive */
  int status;
  pid_t r = waitpid(g_procs[handle].pid, &status, WNOHANG);
  if (r != g_procs[handle].pid) {
    kill(g_procs[handle].pid, SIGKILL);
    waitpid(g_procs[handle].pid, &status, 0);
  }

  close_proc_slot(handle);

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
      close_proc_slot(i);
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

  lua_pushcfunction(L, l_mcp_recv_nowait);
  lua_setfield(L, -2, "recv_nowait");

  lua_pushcfunction(L, l_mcp_alive);
  lua_setfield(L, -2, "alive");

  lua_pushcfunction(L, l_mcp_kill);
  lua_setfield(L, -2, "kill");

  lua_setglobal(L, "mcp");
}
