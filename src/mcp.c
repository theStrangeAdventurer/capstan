#include "tui.h"
#include "utils.h"
#include "process_manager.h"
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
 *   - stderr captured by the process manager (bounded to 64 KiB)
 *
 * recv() reads one line at a time with a timeout, pumping the UI
 * via tui_pump_blocking() so the spinner stays alive.
 */

#define MCP_MAX_PROCS 32

typedef struct {
  char process_id[PROCESS_ID_SIZE];
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
  /* A closed protocol is no longer useful, even if descendants remain. */
  if (g_procs[handle].process_id[0])
    process_manager_stop(g_procs[handle].process_id);
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
  g_procs[handle].process_id[0] = '\0';
}

/* Never reap or signal an adopted PID here. Stopping is already unavailable
 * to the protocol, even while the manager's TERM grace period is running. */
static int proc_alive(int handle) {
  if (handle < 0 || handle >= MCP_MAX_PROCS || !g_procs[handle].alive)
    return 0;
  process_manager_poll();
  ProcessSnapshot snapshot;
  if (process_manager_get(g_procs[handle].process_id, &snapshot) &&
      snapshot.running && !snapshot.stopping)
    return 1;
  close_proc_slot(handle);
  return 0;
}

static int cloexec_pipe(int fds[2]) {
  if (pipe(fds) < 0)
    return -1;
  for (int i = 0; i < 2; i++) {
    /* Keep pipe descriptors away from stdio, including in headless mode. */
    if (fds[i] <= STDERR_FILENO) {
      int fd = fcntl(fds[i], F_DUPFD, STDERR_FILENO + 1);
      if (fd < 0) {
        int err = errno;
        close(fds[0]); close(fds[1]);
        errno = err;
        return -1;
      }
      close(fds[i]);
      fds[i] = fd;
    }
    if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
      int err = errno;
      close(fds[0]);
      close(fds[1]);
      errno = err;
      return -1;
    }
  }
  return 0;
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
  /* Scope comes from the runtime registry, never model tool arguments. */
  const char *owner = luaL_optstring(L, 4, "runtime");
  if (!owner[0]) owner = "runtime";

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
  if (cloexec_pipe(in_pipe) < 0) {
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }
  if (cloexec_pipe(out_pipe) < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }

  int exec_pipe[2];
  if (cloexec_pipe(exec_pipe) < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: pipe() failed: %s", strerror(errno));
  }
  int err_pipe[2];
  if (cloexec_pipe(err_pipe) < 0) {
    int err = errno;
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    close(exec_pipe[0]); close(exec_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: stderr pipe failed: %s", strerror(err));
  }

  pid_t pid = fork();
  if (pid < 0) {
    int err = errno;
    close(err_pipe[0]);
    close(err_pipe[1]);
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(exec_pipe[0]);
    close(exec_pipe[1]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: fork() failed: %s", strerror(err));
  }

  if (pid == 0) {
    /* The child establishes the group before exec; the parent also tries
     * setpgid to cover scheduling races before manager adoption. */
    if (setpgid(0, 0) < 0 ||
        dup2(in_pipe[0], STDIN_FILENO) < 0 ||
        dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(err_pipe[1], STDERR_FILENO) < 0) {
      int err = errno;
      (void)write(exec_pipe[1], &err, sizeof(err));
      _exit(127);
    }
    close(err_pipe[0]);
    close(err_pipe[1]);

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

  /* Parent: adopt before any startup reaping. Protocol pipes remain ours. */
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(exec_pipe[1]);
  close(err_pipe[1]);
  (void)setpgid(pid, pid);
  char process_id[PROCESS_ID_SIZE];
  char workdir[PROCESS_TEXT_SIZE];
  if (!getcwd(workdir, sizeof(workdir)))
    workdir[0] = '\0';
  char saved_owner[sizeof(((ProcessSnapshot *)0)->owner)];
  snprintf(saved_owner, sizeof(saved_owner), "%s", process_manager_owner());
  process_manager_set_owner(owner);
  int adopted = process_manager_adopt_argv(pid, "mcp", argv, workdir, -1,
                                      err_pipe[0], 0, 65536, 0, process_id);
  process_manager_set_owner(saved_owner);
  if (!adopted) {
    /* Adoption failed: this unreaped child still belongs to us. */
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    close(err_pipe[0]); close(exec_pipe[0]);
    close(in_pipe[1]); close(out_pipe[0]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: process manager adoption failed");
  }

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
      process_manager_stop(process_id);
      process_manager_poll();
      free_string_array(argv);
      free_string_array(envp);
      return luaL_error(L, "mcp.spawn: exec failed: %s", strerror(exec_err));
    }
  }
  close(exec_pipe[0]);

  process_manager_poll();
  ProcessSnapshot child;
  if (!process_manager_get(process_id, &child) || !child.running || child.stopping) {
    close(in_pipe[1]);
    close(out_pipe[0]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: process exited during startup");
  }

  /* Set stdout read end to non-blocking for select() */
  int flags = fcntl(out_pipe[0], F_GETFL, 0);
  if (flags < 0 || fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
    int err = errno;
    process_manager_stop(process_id);
    close(in_pipe[1]); close(out_pipe[0]);
    free_string_array(argv);
    free_string_array(envp);
    return luaL_error(L, "mcp.spawn: nonblocking stdout failed: %s", strerror(err));
  }

  memcpy(g_procs[slot].process_id, process_id, sizeof(process_id));
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

  if (!proc_alive(handle)) {
    lua_pushnil(L);
    lua_pushstring(L, "mcp.send: process stopped or exited; reconnect MCP server");
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

    /* Manager owns child status and signal escalation. */
    if (!proc_alive(handle)) {
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

  if (!proc_alive(handle)) {
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
  lua_pushboolean(L, proc_alive(handle));
  return 1;
}

static int l_mcp_kill(lua_State *L) {
  int handle = (int)luaL_optinteger(L, 1, -1);
  if (handle < 0 || handle >= MCP_MAX_PROCS || !g_procs[handle].alive) {
    lua_pushboolean(L, 0);
    return 1;
  }

  /* Asynchronous TERM/escalation: never block or reenter Lua through UI. */
  process_manager_stop(g_procs[handle].process_id);
  close_proc_slot(handle);

  lua_pushboolean(L, 1);
  return 1;
}

/* Kill all alive procs — called at shutdown */
void mcp_cleanup(void) {
  for (int i = 0; i < MCP_MAX_PROCS; i++) {
    if (g_procs[i].alive) {
      process_manager_stop(g_procs[i].process_id);
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
