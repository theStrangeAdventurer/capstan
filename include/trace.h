#ifndef TRACE_H
#define TRACE_H

#include <stddef.h>

#ifndef TRACE_PATH_SIZE
#define TRACE_PATH_SIZE 1024
#endif

typedef enum {
  TRACE_STATE_DISABLED = 0,
  TRACE_STATE_ACTIVE,
  TRACE_STATE_TERMINAL,
  TRACE_STATE_PUBLISHED,
  TRACE_STATE_FAILED,
  TRACE_STATE_ABORTED,
} TraceState;

typedef struct {
  int fd;
  int dir_fd;
  int lock_fd;
  long long started_wall_ms;
  long long started_monotonic_ms;
  unsigned long seq;
  int failure_errno;
  int terminal_written;
  TraceState state;
  unsigned long long partial_dev;
  unsigned long long partial_ino;
  char run_id[64];
  char target_name[512];
  char partial_name[520];
  char lock_name[520];
  char target_path[TRACE_PATH_SIZE];
  char partial_path[TRACE_PATH_SIZE];
} TraceWriter;

#define TRACE_WRITER_INIT \
  {.fd = -1, .dir_fd = -1, .lock_fd = -1, .state = TRACE_STATE_DISABLED}

int trace_open(TraceWriter *trace, const char *path, char *error,
               size_t error_size);
int trace_event(TraceWriter *trace, const char *event, const char *data_json);
int trace_tool_event(TraceWriter *trace, const char *event, const char *name,
                     int has_result, int ok, long long wall_duration_ms,
                     long long permission_wait_ms, size_t result_bytes);
int trace_terminal_event(TraceWriter *trace, const char *event,
                         const char *data_json);
int trace_finish(TraceWriter *trace, char *error, size_t error_size);
void trace_close(TraceWriter *trace);
long long trace_now_ms(void);
long long trace_monotonic_ms(void);
int trace_failed(const TraceWriter *trace);
int trace_is_active(const TraceWriter *trace);
void trace_accumulate_tool_breakdown(int is_subagent,
                                     long long wall_duration_ms,
                                     long long permission_wait_ms,
                                     long long *tool_ms,
                                     long long *permission_ms,
                                     long long *subagent_ms);

#endif
