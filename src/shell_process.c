#include "shell_process.h"
#include "process_manager.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int process_run(const char *command, char *const argv[],
                       const char *workdir, int timeout_sec,
                       size_t max_stdout, size_t max_stderr,
                       ShellProcessPump pump, ShellProcessResult *result) {
  if ((!command && (!argv || !argv[0])) || !workdir || timeout_sec <= 0 || !result)
    return 0;
  memset(result, 0, sizeof(*result));
  result->exit_code = -1;

  char id[PROCESS_ID_SIZE];
  if (!process_manager_start(command, argv, workdir, timeout_sec,
                             max_stdout, max_stderr, id)) return 0;
  ProcessSnapshot snapshot;
  for (;;) {
    process_manager_poll();
    if (!process_manager_get(id, &snapshot)) return 0;
    if (!snapshot.running) break;
    if (pump) pump();
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
  result->exit_code = snapshot.exit_code;
  result->timed_out = snapshot.timed_out;
  result->stdout_text = process_manager_raw_output(id, 0);
  result->stderr_text = process_manager_raw_output(id, 1);
  if (!result->stdout_text || !result->stderr_text) {
    shell_process_result_free(result);
    return 0;
  }
  return 1;
}

int shell_process_run(const char *command, const char *workdir, int timeout_sec,
                      size_t max_stdout, size_t max_stderr,
                      ShellProcessPump pump, ShellProcessResult *result) {
  return process_run(command, NULL, workdir, timeout_sec, max_stdout,
                     max_stderr, pump, result);
}

int shell_process_run_argv(char *const argv[], const char *workdir,
                           int timeout_sec, size_t max_stdout,
                           size_t max_stderr, ShellProcessPump pump,
                           ShellProcessResult *result) {
  return process_run(NULL, argv, workdir, timeout_sec, max_stdout,
                     max_stderr, pump, result);
}

void shell_process_result_free(ShellProcessResult *result) {
  if (!result)
    return;
  free(result->stdout_text);
  free(result->stderr_text);
  result->stdout_text = NULL;
  result->stderr_text = NULL;
}
