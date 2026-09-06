#include "workspace_status.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STATUS_MAX_OUTPUT (1024 * 1024)
#define STATUS_REFRESH_MS 2000
#define STATUS_TIMEOUT_MS 5000

static int add_number(const char **cursor, unsigned long long *total) {
  const char *p = *cursor;
  if (*p == '-' && p[1] == '\t') { *cursor = p + 2; return 1; }
  if (*p < '0' || *p > '9') return 0;
  errno = 0;
  char *end;
  unsigned long long value = strtoull(p, &end, 10);
  if (errno || *end != '\t' || ULLONG_MAX - *total < value) return 0;
  *total += value;
  *cursor = end + 1;
  return 1;
}

int workspace_status_parse(const char *output, WorkspaceStatus *status) {
  WorkspaceStatus parsed = {.state = WORKSPACE_STATUS_READY};
  *status = (WorkspaceStatus){.state = WORKSPACE_STATUS_ERROR};
  if (!output) return 0;
  if (strcmp(output, "none\n") == 0) {
    status->state = WORKSPACE_STATUS_NONE;
    return 1;
  }
  if (strncmp(output, "repo\n", 5) != 0) return 0;
  const char *p = output + 5;
  while (strncmp(p, "stats\n", 6) != 0) {
    const char *end = strchr(p, '\n');
    if (!end || end - p < 4 || p[2] != ' ' ||
        !strchr(" MADRCU?!T", p[0]) || !strchr(" MADRCU?!T", p[1])) return 0;
    parsed.files++;
    p = end + 1;
  }
  p += 6;
  while (strcmp(p, "done\n") != 0) {
    if (!add_number(&p, &parsed.added) || !add_number(&p, &parsed.deleted)) return 0;
    const char *end = strchr(p, '\n');
    if (!end || end == p) return 0;
    p = end + 1;
  }
  *status = parsed;
  return 1;
}

int workspace_status_parse_files(const char *output, WorkspaceStatus *status) {
  WorkspaceStatus parsed = {.state = WORKSPACE_STATUS_READY};
  *status = (WorkspaceStatus){.state = WORKSPACE_STATUS_ERROR};
  if (!output) return 0;
  if (strcmp(output, "none\n") == 0) {
    status->state = WORKSPACE_STATUS_NONE;
    return 1;
  }
  if (strncmp(output, "files-v1\n", 9) != 0) return 0;
  const char *p = output + 9;
  while (strcmp(p, "done\n") != 0) {
    if (!strchr("MADRCUT?", *p) || !*p || p[1] != '\t') return 0;
    p += 2;
    if (!add_number(&p, &parsed.added) || !add_number(&p, &parsed.deleted)) return 0;
    const char *end = strchr(p, '\n');
    if (!end || end == p) return 0;
    /* Paths are escaped display labels; raw control characters are invalid. */
    for (const char *q = p; q < end; q++)
      if ((unsigned char)*q < 32 || *q == 127) return 0;
    parsed.files++;
    p = end + 1;
  }
  *status = parsed;
  return 1;
}

static struct {
  WorkspaceStatus value;
  char *adapter;
  char **argv;
  int git_format;
  char *workspace, *output;
  size_t length;
  pid_t pid;
  int fd;
  long long started, next;
} monitor = {.fd = -1};

static long long now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void stop_worker(void) {
  if (monitor.pid > 0) {
    /* The shell and Git share a dedicated process group. */
    kill(-monitor.pid, SIGKILL);
    kill(monitor.pid, SIGKILL);
    while (waitpid(monitor.pid, NULL, 0) < 0 && errno == EINTR) {}
    monitor.pid = 0;
  }
  if (monitor.fd >= 0) close(monitor.fd);
  monitor.fd = -1;
  free(monitor.output);
  monitor.output = NULL;
  monitor.length = 0;
}

void workspace_status_shutdown(void) {
  stop_worker();
  free(monitor.workspace);
  monitor.workspace = NULL;
  monitor.value = (WorkspaceStatus){.state = WORKSPACE_STATUS_PENDING};
  monitor.next = 0;
  free(monitor.adapter);
  monitor.adapter = NULL;
  if (monitor.argv) {
    for (size_t i = 0; monitor.argv[i]; i++) free(monitor.argv[i]);
    free(monitor.argv);
    monitor.argv = NULL;
  }
}

static char *copy_string(const char *s) {
  char *copy = malloc(strlen(s) + 1);
  if (copy) strcpy(copy, s);
  return copy;
}

void workspace_status_configure(const char *adapter, const char *const *argv,
                                int git_format) {
  if (!adapter) adapter = "";
  int same = monitor.adapter && strcmp(monitor.adapter, adapter) == 0 &&
             monitor.git_format == git_format;
  size_t count = 0;
  if (argv) while (argv[count]) count++;
  if (same) {
    size_t i = 0;
    while (monitor.argv && monitor.argv[i] && i < count &&
           strcmp(monitor.argv[i], argv[i]) == 0) i++;
    if (i == count && (!monitor.argv || !monitor.argv[i])) return;
  }
  workspace_status_shutdown();
  monitor.git_format = git_format;
  monitor.adapter = copy_string(adapter);
  if (!monitor.adapter) goto failed;
  if (!count) {
    monitor.value.state = WORKSPACE_STATUS_ERROR;
    return;
  }
  monitor.argv = calloc(count + 1, sizeof(char *));
  if (!monitor.argv) goto failed;
  for (size_t i = 0; i < count; i++) {
    monitor.argv[i] = copy_string(argv[i]);
    if (!monitor.argv[i]) goto failed;
  }
  return;
failed:
  workspace_status_shutdown();
  monitor.value.state = WORKSPACE_STATUS_ERROR;
}

static int start_worker(const char *workspace, long long now) {
  int fds[2];
  if (pipe(fds) != 0) return 0;
  if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
    close(fds[0]); close(fds[1]); return 0;
  }
  monitor.output = malloc(STATUS_MAX_OUTPUT + 1);
  if (!monitor.output) { close(fds[0]); close(fds[1]); return 0; }
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 ||
        dup2(nullfd, STDERR_FILENO) < 0 || dup2(fds[1], STDOUT_FILENO) < 0)
      _exit(127);
    if (nullfd > STDERR_FILENO) close(nullfd);
    close(fds[0]); close(fds[1]);
    if (chdir(workspace) != 0) _exit(127);
    execvp(monitor.argv[0], monitor.argv);
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]); free(monitor.output); monitor.output = NULL; return 0;
  }
  setpgid(pid, pid);
  monitor.pid = pid;
  monitor.fd = fds[0];
  monitor.length = 0;
  monitor.started = now;
  return 1;
}

const WorkspaceStatus *workspace_status_poll(const char *workspace) {
  long long now = now_ms();
  if (!monitor.argv) {
    monitor.value = (WorkspaceStatus){.state = WORKSPACE_STATUS_ERROR};
    return &monitor.value;
  }
  if (!workspace) workspace = "";
  if (!monitor.workspace || strcmp(workspace, monitor.workspace) != 0) {
    stop_worker();
    free(monitor.workspace);
    monitor.value = (WorkspaceStatus){.state = WORKSPACE_STATUS_PENDING};
    monitor.next = 0;
    monitor.workspace = malloc(strlen(workspace) + 1);
    if (!monitor.workspace) { monitor.value.state = WORKSPACE_STATUS_ERROR; return &monitor.value; }
    strcpy(monitor.workspace, workspace);
  }
  if (monitor.pid > 0) {
    int failed = now - monitor.started >= STATUS_TIMEOUT_MS;
    /* Bound work per frame, even if a huge repository fills the pipe. */
    for (int i = 0; !failed && monitor.fd >= 0 && i < 16; i++) {
      size_t available = STATUS_MAX_OUTPUT - monitor.length;
      ssize_t n = read(monitor.fd, monitor.output + monitor.length,
                       available < 4096 ? available : 4096);
      if (n > 0) {
        monitor.length += (size_t)n;
        if (monitor.length == STATUS_MAX_OUTPUT) failed = 1;
      } else if (n == 0) {
        close(monitor.fd); monitor.fd = -1;
      } else if (errno == EAGAIN || errno == EINTR) {
        break;
      } else {
        failed = 1;
      }
    }
    int status = 0;
    pid_t done = monitor.fd < 0 ? waitpid(monitor.pid, &status, WNOHANG) : 0;
    if (failed || done != 0) {
      if (!failed && done == monitor.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        monitor.output[monitor.length] = '\0';
        if (memchr(monitor.output, '\0', monitor.length))
          monitor.value = (WorkspaceStatus){.state = WORKSPACE_STATUS_ERROR};
        else if (monitor.git_format)
          workspace_status_parse(monitor.output, &monitor.value);
        else
          workspace_status_parse_files(monitor.output, &monitor.value);
      } else {
        monitor.value = (WorkspaceStatus){.state = WORKSPACE_STATUS_ERROR};
      }
      if (done == monitor.pid) monitor.pid = 0;
      stop_worker();
      monitor.next = now + STATUS_REFRESH_MS;
    }
  } else if (now >= monitor.next) {
    if (!start_worker(workspace, now)) {
      monitor.value.state = WORKSPACE_STATUS_ERROR;
      monitor.next = now + STATUS_REFRESH_MS;
    }
  }
  return &monitor.value;
}
