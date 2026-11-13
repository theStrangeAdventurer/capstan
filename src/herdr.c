#include "herdr.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static pid_t child;
static double started;
static double retry_after;
static char *pending_session;
static const char *pending_state;

static double monotonic_now(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec + now.tv_nsec / 1e9;
}
static char *last_session;
static const char *last_state;
static int reported;
static int cleanup_registered;

static int enabled(void) {
  const char *env = getenv("HERDR_ENV");
  const char *names[] = {"HERDR_BIN_PATH", "HERDR_PANE_ID", "HERDR_SOCKET_PATH"};
  if (!env || strcmp(env, "1")) return 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    const char *value = getenv(names[i]);
    if (!value || !*value) return 0;
  }
  return 1;
}

static pid_t send_report(const char *session, const char *state) {
  char *argv[] = {getenv("HERDR_BIN_PATH"), "pane",
    state ? "report-agent" : "release-agent", getenv("HERDR_PANE_ID"),
    "--source", "capstan", "--agent", "capstan", NULL, NULL, NULL, NULL, NULL};
  posix_spawn_file_actions_t actions;
  pid_t pid = 0;
  if (state) {
    argv[8] = "--state";
    argv[9] = (char *)state;
    if (session && *session) {
      argv[10] = "--agent-session-id";
      argv[11] = (char *)session;
    }
  }
  if (posix_spawn_file_actions_init(&actions)) return 0;
  int error = 0;
  for (int fd = 0; fd <= 2 && !error; fd++)
    error = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null",
                                          fd ? O_WRONLY : O_RDONLY, 0);
  if (!error)
    error = posix_spawn(&pid, argv[0], &actions, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  return error ? 0 : pid;
}

void herdr_tick(const char *session_id, const char *state) {
  if (!enabled()) return;
  if (!cleanup_registered) {
    if (atexit(herdr_shutdown) != 0) return;
    cleanup_registered = 1;
  }
  double now = monotonic_now();
  if (child) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (!result || (result < 0 && errno == EINTR)) {
      if (now - started >= 2) kill(child, SIGKILL);
      return;
    }
    if (result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      free(last_session);
      last_session = pending_session;
      pending_session = NULL;
      last_state = pending_state;
    } else {
      retry_after = now + 1;
    }
    free(pending_session);
    pending_session = NULL;
    pending_state = NULL;
    child = 0;
  }
  if (!state || now < retry_after) return;
  if (!strcmp(state, "working")) state = "working";
  else if (!strcmp(state, "idle")) state = "idle";
  else if (!strcmp(state, "blocked")) state = "blocked";
  else return;
  if (!session_id) session_id = "";
  if (last_state && !strcmp(last_state, state) && last_session &&
      !strcmp(last_session, session_id))
    return;
  char *copy = malloc(strlen(session_id) + 1);
  if (!copy) return;
  strcpy(copy, session_id);
  child = send_report(session_id, state);
  if (!child) {
    free(copy);
    retry_after = now + 1;
    return;
  }
  pending_session = copy;
  pending_state = state;
  started = now;
  reported = 1;
}

static void finish_on_shutdown(pid_t pid) {
  if (!pid) return;
  for (int i = 0; i < 20; i++) {
    pid_t result = waitpid(pid, NULL, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) return;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, WNOHANG);
}

void herdr_shutdown(void) {
  finish_on_shutdown(child);
  child = 0;
  /* A session leader exiting can SIGHUP the release command: let it finish. */
  if (reported && enabled()) finish_on_shutdown(send_report(NULL, NULL));
  free(last_session);
  free(pending_session);
  last_session = NULL;
  pending_session = NULL;
  pending_state = NULL;
  last_state = NULL;
  retry_after = 0;
  reported = 0;
}
