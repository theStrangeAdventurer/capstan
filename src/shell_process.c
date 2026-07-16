#include "shell_process.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void kill_process_group(pid_t pid, int signal_number) {
  if (kill(-pid, signal_number) != 0)
    kill(pid, signal_number);
}

static void read_pipe(int fd, int *open, char *buffer, size_t *length,
                      size_t maximum) {
  char chunk[4096];
  ssize_t count = read(fd, chunk, sizeof(chunk));
  if (count <= 0) {
    close(fd);
    *open = 0;
    return;
  }
  size_t available = maximum > *length ? maximum - *length : 0;
  size_t copy = (size_t)count < available ? (size_t)count : available;
  if (copy > 0) {
    memcpy(buffer + *length, chunk, copy);
    *length += copy;
    buffer[*length] = '\0';
  }
}

int shell_process_run(const char *command, const char *workdir, int timeout_sec,
                      size_t max_stdout, size_t max_stderr,
                      ShellProcessPump pump, ShellProcessResult *result) {
  if (!command || !workdir || timeout_sec <= 0 || !result)
    return 0;
  memset(result, 0, sizeof(*result));
  result->exit_code = -1;

  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0)
    return 0;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return 0;
  }

  result->stdout_text = calloc(max_stdout + 1, 1);
  result->stderr_text = calloc(max_stderr + 1, 1);
  if (!result->stdout_text || !result->stderr_text) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    shell_process_result_free(result);
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    shell_process_result_free(result);
    return 0;
  }
  if (pid == 0) {
    setpgid(0, 0);
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
    if (chdir(workdir) != 0)
      _exit(127);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }

  setpgid(pid, pid);
  close(out_pipe[1]);
  close(err_pipe[1]);

  size_t out_len = 0;
  size_t err_len = 0;
  int out_open = 1;
  int err_open = 1;
  int child_done = 0;
  int status = 0;
  long long started = now_ms();
  long long deadline = started + (long long)timeout_sec * 1000LL;
  long long forced_deadline = 0;
  long long drain_deadline = 0;

  while (out_open || err_open || !child_done) {
    long long now = now_ms();
    if (!result->timed_out && now >= deadline) {
      result->timed_out = 1;
      kill_process_group(pid, SIGTERM);
      forced_deadline = now + 500;
      drain_deadline = now + 2000;
    } else if (result->timed_out && forced_deadline > 0 && now >= forced_deadline) {
      kill_process_group(pid, SIGKILL);
      forced_deadline = 0;
    }

    if (result->timed_out && drain_deadline > 0 && now >= drain_deadline) {
      if (out_open) {
        close(out_pipe[0]);
        out_open = 0;
      }
      if (err_open) {
        close(err_pipe[0]);
        err_open = 0;
      }
      drain_deadline = 0;
    }

    if (!child_done) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid || (waited < 0 && errno == ECHILD))
        child_done = 1;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    int maxfd = -1;
    if (out_open) {
      FD_SET(out_pipe[0], &read_set);
      maxfd = out_pipe[0];
    }
    if (err_open) {
      FD_SET(err_pipe[0], &read_set);
      if (err_pipe[0] > maxfd)
        maxfd = err_pipe[0];
    }
    struct timeval wait = {.tv_sec = 0, .tv_usec = 100000};
    int ready = maxfd >= 0 ? select(maxfd + 1, &read_set, NULL, NULL, &wait) : 0;
    if (ready > 0) {
      if (out_open && FD_ISSET(out_pipe[0], &read_set))
        read_pipe(out_pipe[0], &out_open, result->stdout_text, &out_len,
                  max_stdout);
      if (err_open && FD_ISSET(err_pipe[0], &read_set))
        read_pipe(err_pipe[0], &err_open, result->stderr_text, &err_len,
                  max_stderr);
    } else if (ready < 0 && errno != EINTR) {
      break;
    } else if (maxfd < 0) {
      usleep(100000);
    }
    if (pump)
      pump();
  }

  if (!child_done) {
    kill_process_group(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (WIFEXITED(status))
    result->exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    result->exit_code = 128 + WTERMSIG(status);

  if (out_open)
    close(out_pipe[0]);
  if (err_open)
    close(err_pipe[0]);
  return 1;
}

void shell_process_result_free(ShellProcessResult *result) {
  if (!result)
    return;
  free(result->stdout_text);
  free(result->stderr_text);
  result->stdout_text = NULL;
  result->stderr_text = NULL;
}
