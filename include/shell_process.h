#ifndef SHELL_PROCESS_H
#define SHELL_PROCESS_H

#include <stddef.h>

typedef void (*ShellProcessPump)(void);

typedef struct {
  int exit_code;
  int timed_out;
  char *stdout_text;
  char *stderr_text;
} ShellProcessResult;

int shell_process_run(const char *command, const char *workdir, int timeout_sec,
                      size_t max_stdout, size_t max_stderr,
                      ShellProcessPump pump, ShellProcessResult *result);
int shell_process_run_argv(char *const argv[], const char *workdir,
                           int timeout_sec, size_t max_stdout,
                           size_t max_stderr, ShellProcessPump pump,
                           ShellProcessResult *result);
void shell_process_result_free(ShellProcessResult *result);

#endif
