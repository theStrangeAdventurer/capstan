#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H
#include <stddef.h>
#include <sys/types.h>

/* Runtime-local opaque IDs, never PID aliases. Snapshots own their strings. */
#define PROCESS_ID_SIZE 64
#define PROCESS_TEXT_SIZE 512
typedef struct {
  char id[PROCESS_ID_SIZE];
  pid_t pid, pgid;
  char kind[24], label[PROCESS_TEXT_SIZE], workdir[PROCESS_TEXT_SIZE];
  char owner[128];
  char task_id[PROCESS_ID_SIZE];
  int running, stopping, timed_out, exit_code;
  long long started_ms, finished_ms;
  int output_available, truncated;
} ProcessSnapshot;

/* Set by runtime adapters, not model arguments. Copied when a process starts. */
void process_manager_set_owner(const char *owner);
/* Borrowed, sanitized current owner; valid until the next set_owner call. */
const char *process_manager_owner(void);
void process_manager_set_task(const char *task_id);
const char *process_manager_task(void);
void process_manager_stop_task(const char *task_id);
/* Adopt a direct, unreaped child with its own process group. Manager owns waitpid
 * and signals from this point. out_fd/err_fd are transferred (-1: protocol owned).
 * Returns 0 on failure; caller still owns child and descriptors in that case. */
int process_manager_adopt(pid_t pid, const char *kind, const char *label,
                          const char *workdir, int out_fd, int err_fd,
                          size_t max_stdout, size_t max_stderr, int timeout_sec,
                          char id[PROCESS_ID_SIZE]);
/* Same adoption contract, with a sanitized command-and-arguments label. */
int process_manager_adopt_argv(pid_t pid, const char *kind, char *const argv[],
                               const char *workdir, int out_fd, int err_fd,
                               size_t max_stdout, size_t max_stderr, int timeout_sec,
                               char id[PROCESS_ID_SIZE]);
int process_manager_start(const char *command, char *const argv[],
                          const char *workdir, int timeout_sec,
                          size_t max_stdout, size_t max_stderr,
                          char id[PROCESS_ID_SIZE]);
void process_manager_poll(void);
size_t process_manager_count(void);
int process_manager_at(size_t index, ProcessSnapshot *snapshot);
int process_manager_get(const char *id, ProcessSnapshot *snapshot);
int process_manager_stop(const char *id);
/* Background completion delivery is bounded by retained records; no callbacks. */
void process_manager_watch(const char *id);
int process_manager_completion(const char *owner, ProcessSnapshot *snapshot);
void process_manager_stop_owner(const char *owner);
/* Bounded owner teardown; returns false if any owned process remains live. */
int process_manager_close_owner(const char *owner);
/* Caller frees output. Public output is redacted and terminal-control safe.
 * Open streams expose completed sanitized lines; EOF exposes the final partial
 * line. Overflow retains the bounded prefix, with a truncation marker appended.
 * Sanitization/redaction precedes public bounds and metadata bounded copies. */
char *process_manager_output(const char *id, int stderr_stream);
/* Only synchronous shell compatibility uses raw output; never render it. */
char *process_manager_raw_output(const char *id, int stderr_stream);
void process_manager_shutdown(void);
long long process_manager_now_ms(void);
#endif
