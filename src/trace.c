#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "trace.h"
#include "jsonl.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

long long trace_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

long long trace_monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return trace_now_ms();
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void set_error(char *error, size_t error_size, const char *message,
                      const char *path, int error_number) {
  if (!error || error_size == 0)
    return;
  if (path)
    snprintf(error, error_size, "%s %s: %s", message, path,
             strerror(error_number));
  else
    snprintf(error, error_size, "%s: %s", message, strerror(error_number));
}

static void release_handles(TraceWriter *trace) {
  if (trace->fd >= 0) {
    (void)close(trace->fd);
    trace->fd = -1;
  }
  if (trace->lock_fd >= 0) {
    struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
    (void)fcntl(trace->lock_fd, F_SETLK, &lock);
    (void)close(trace->lock_fd);
    trace->lock_fd = -1;
  }
  if (trace->dir_fd >= 0) {
    (void)close(trace->dir_fd);
    trace->dir_fd = -1;
  }
}

static void fail_trace(TraceWriter *trace, int error_number) {
  if (!trace)
    return;
  if (!trace->failure_errno)
    trace->failure_errno = error_number ? error_number : EIO;
  trace->state = TRACE_STATE_FAILED;
}

static int split_trace_path(const char *path, char *parent,
                            size_t parent_size, char *name,
                            size_t name_size) {
  char absolute[TRACE_PATH_SIZE];
  if (path[0] == '/') {
    if (snprintf(absolute, sizeof(absolute), "%s", path) >=
        (int)sizeof(absolute))
      return 0;
  } else {
    char cwd[TRACE_PATH_SIZE];
    if (!getcwd(cwd, sizeof(cwd)) ||
        snprintf(absolute, sizeof(absolute), "%s/%s", cwd, path) >=
            (int)sizeof(absolute))
      return 0;
  }

  char *slash = strrchr(absolute, '/');
  if (!slash || !slash[1])
    return 0;
  if (strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0)
    return 0;
  if (snprintf(name, name_size, "%s", slash + 1) >= (int)name_size)
    return 0;
  if (slash == absolute)
    slash[1] = '\0';
  else
    *slash = '\0';

  char resolved[TRACE_PATH_SIZE];
  if (!realpath(absolute, resolved) ||
      snprintf(parent, parent_size, "%s", resolved) >= (int)parent_size)
    return 0;
  return 1;
}

static int open_directory_nofollow(const char *absolute_path) {
  if (!absolute_path || absolute_path[0] != '/') {
    errno = EINVAL;
    return -1;
  }

  int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return -1;

  const char *cursor = absolute_path;
  while (*cursor == '/')
    cursor++;
  while (*cursor) {
    const char *end = strchr(cursor, '/');
    size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
    char component[TRACE_PATH_SIZE];
    if (length == 0 || length >= sizeof(component)) {
      close(fd);
      errno = ENAMETOOLONG;
      return -1;
    }
    memcpy(component, cursor, length);
    component[length] = '\0';

    int next = openat(fd, component,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0) {
      int saved = errno;
      close(fd);
      errno = saved;
      return -1;
    }
    close(fd);
    fd = next;
    if (!end)
      break;
    cursor = end;
    while (*cursor == '/')
      cursor++;
  }
  return fd;
}

static int leaf_status(int dir_fd, const char *name, struct stat *st) {
  if (fstatat(dir_fd, name, st, AT_SYMLINK_NOFOLLOW) == 0)
    return 1;
  return errno == ENOENT ? 0 : -1;
}

static int validate_trace_directory(const struct stat *st, const char *path,
                                    char *error, size_t error_size) {
  if (!st || !S_ISDIR(st->st_mode)) {
    errno = ENOTDIR;
    set_error(error, error_size, "invalid trace directory", path, errno);
    return 0;
  }
  mode_t shared_write = st->st_mode & (S_IWGRP | S_IWOTH);
  if (shared_write && !(st->st_mode & S_ISVTX)) {
    errno = EPERM;
    if (error && error_size)
      snprintf(error, error_size,
               "trace directory is writable by other users without sticky bit: %s",
               path);
    return 0;
  }
  return 1;
}

static int validate_regular_leaf(int dir_fd, const char *name,
                                 const char *path, char *error,
                                 size_t error_size) {
  struct stat st;
  int status = leaf_status(dir_fd, name, &st);
  if (status == 0)
    return 1;
  if (status < 0) {
    set_error(error, error_size, "cannot inspect trace file", path, errno);
    return 0;
  }
  if (!S_ISREG(st.st_mode)) {
    if (error && error_size)
      snprintf(error, error_size, "trace path is not a regular file: %s",
               path);
    return 0;
  }
  return 1;
}

static int owns_trace_leaf(const TraceWriter *trace, const char *name,
                           int require_single_link) {
  if (!trace || trace->dir_fd < 0 || !name || !trace->partial_dev ||
      !trace->partial_ino)
    return 0;
  struct stat current;
  return fstatat(trace->dir_fd, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(current.st_mode) &&
         (!require_single_link || current.st_nlink == 1) &&
         (unsigned long long)current.st_dev == trace->partial_dev &&
         (unsigned long long)current.st_ino == trace->partial_ino;
}

static int owns_partial(const TraceWriter *trace) {
  return owns_trace_leaf(trace, trace ? trace->partial_name : NULL, 1);
}

int trace_open(TraceWriter *trace, const char *path, char *error,
               size_t error_size) {
  if (error && error_size)
    error[0] = '\0';
  if (!trace || !path || !path[0])
    return 0;
  if (trace->state == TRACE_STATE_ACTIVE ||
      trace->state == TRACE_STATE_TERMINAL || trace->fd >= 0 ||
      trace->dir_fd >= 0 || trace->lock_fd >= 0) {
    if (error && error_size)
      snprintf(error, error_size, "trace writer is already active");
    errno = EBUSY;
    return 0;
  }
  memset(trace, 0, sizeof(*trace));
  trace->fd = -1;
  trace->dir_fd = -1;
  trace->lock_fd = -1;
  trace->state = TRACE_STATE_FAILED;

  char parent[TRACE_PATH_SIZE];
  if (!split_trace_path(path, parent, sizeof(parent), trace->target_name,
                        sizeof(trace->target_name))) {
    if (error && error_size)
      snprintf(error, error_size,
               "trace path must name a file in an existing directory: %s",
               path);
    trace->failure_errno = EINVAL;
    return 0;
  }
  const char *reserved_suffix = ".partial";
  size_t target_length = strlen(trace->target_name);
  size_t suffix_length = strlen(reserved_suffix);
  if (target_length >= suffix_length &&
      strcmp(trace->target_name + target_length - suffix_length,
             reserved_suffix) == 0) {
    if (error && error_size)
      snprintf(error, error_size,
               "trace path must not use the reserved .partial suffix: %s",
               path);
    trace->failure_errno = EINVAL;
    return 0;
  }
  if (snprintf(trace->partial_name, sizeof(trace->partial_name), "%s.partial",
               trace->target_name) >= (int)sizeof(trace->partial_name) ||
      snprintf(trace->lock_name, sizeof(trace->lock_name), "%s.lock",
               trace->target_name) >= (int)sizeof(trace->lock_name) ||
      snprintf(trace->target_path, sizeof(trace->target_path), "%s/%s",
               parent, trace->target_name) >= (int)sizeof(trace->target_path) ||
      snprintf(trace->partial_path, sizeof(trace->partial_path), "%s/%s",
               parent, trace->partial_name) >= (int)sizeof(trace->partial_path)) {
    if (error && error_size)
      snprintf(error, error_size, "trace path is too long");
    trace->failure_errno = ENAMETOOLONG;
    return 0;
  }

  trace->dir_fd = open_directory_nofollow(parent);
  if (trace->dir_fd < 0) {
    trace->failure_errno = errno;
    set_error(error, error_size, "cannot open trace directory", parent,
              errno);
    return 0;
  }
  struct stat dir_st;
  if (fstat(trace->dir_fd, &dir_st) != 0 ||
      !validate_trace_directory(&dir_st, parent, error, error_size)) {
    int saved = errno ? errno : ENOTDIR;
    release_handles(trace);
    trace->failure_errno = saved;
    if (error && error_size && !error[0])
      set_error(error, error_size, "invalid trace directory", parent, saved);
    return 0;
  }

  if (!validate_regular_leaf(trace->dir_fd, trace->target_name,
                             trace->target_path, error, error_size)) {
    int saved = errno ? errno : EINVAL;
    release_handles(trace);
    trace->failure_errno = saved;
    return 0;
  }

  trace->fd = openat(trace->dir_fd, trace->partial_name,
                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                     0600);
  if (trace->fd < 0) {
    int saved = errno;
    release_handles(trace);
    trace->failure_errno = saved;
    if (saved == EEXIST && error && error_size)
      snprintf(error, error_size,
               "trace target is already in use or has a stale partial: %s",
               trace->target_path);
    else
      set_error(error, error_size, "cannot create trace file",
                trace->partial_path, saved);
    return 0;
  }
  struct stat partial_st;
  if (fstat(trace->fd, &partial_st) != 0 ||
      !S_ISREG(partial_st.st_mode) || partial_st.st_nlink != 1 ||
      fchmod(trace->fd, 0600) != 0) {
    int saved = errno ? errno : EINVAL;
    (void)unlinkat(trace->dir_fd, trace->partial_name, 0);
    release_handles(trace);
    trace->failure_errno = saved;
    set_error(error, error_size, "cannot secure trace file",
              trace->partial_path, saved);
    return 0;
  }

  trace->partial_dev = (unsigned long long)partial_st.st_dev;
  trace->partial_ino = (unsigned long long)partial_st.st_ino;
  trace->started_wall_ms = trace_now_ms();
  trace->started_monotonic_ms = trace_monotonic_ms();
  snprintf(trace->run_id, sizeof(trace->run_id), "%lld-%ld-%lld",
           trace->started_wall_ms, (long)getpid(),
           trace->started_monotonic_ms);
  trace->state = TRACE_STATE_ACTIVE;
  trace->failure_errno = 0;
  return 1;
}

static int write_event(TraceWriter *trace, const char *event,
                       const char *data_json, const char *tool_name,
                       int has_result, int ok, long long wall_duration_ms,
                       long long permission_wait_ms, size_t result_bytes) {
  if (!trace || trace->state == TRACE_STATE_DISABLED)
    return 1;
  if (trace->state != TRACE_STATE_ACTIVE || trace->fd < 0)
    return 0;
  if (!owns_partial(trace)) {
    fail_trace(trace, ESTALE);
    return 0;
  }

  JsonlBuffer line;
  jsonl_buffer_init(&line);
  unsigned long next_seq = trace->seq + 1;
  jsonl_append_format(
      &line,
      "{\"schema\":\"capstan.trace.v1\",\"seq\":%lu,"
      "\"timestamp_ms\":%lld,\"elapsed_ms\":%lld,\"run_id\":",
      next_seq, trace_now_ms(),
      trace_monotonic_ms() - trace->started_monotonic_ms);
  jsonl_append_string(&line, trace->run_id);
  jsonl_append(&line, ",\"event\":");
  jsonl_append_string(&line, event ? event : "unknown");
  if (tool_name) {
    long long execution_ms = wall_duration_ms - permission_wait_ms;
    if (execution_ms < 0)
      execution_ms = 0;
    jsonl_append(&line, ",\"data\":{\"name\":");
    jsonl_append_string(&line, tool_name);
    if (has_result)
      jsonl_append_format(
          &line,
          ",\"ok\":%s,\"wall_duration_ms\":%lld,"
          "\"execution_ms\":%lld,\"permission_wait_ms\":%lld,"
          "\"result_bytes\":%zu",
          ok ? "true" : "false", wall_duration_ms, execution_ms,
          permission_wait_ms, result_bytes);
    jsonl_append(&line, "}}");
  } else {
    jsonl_append_format(&line, ",\"data\":%s}",
                        data_json ? data_json : "{}");
  }

  off_t original_size = lseek(trace->fd, 0, SEEK_CUR);
  if (original_size < 0) {
    int saved = errno;
    jsonl_buffer_free(&line);
    fail_trace(trace, saved);
    return 0;
  }
  int written = jsonl_write_line(trace->fd, &line);
  jsonl_buffer_free(&line);
  if (!written) {
    fail_trace(trace, errno);
    return 0;
  }
  if (!owns_partial(trace)) {
    int saved = ESTALE;
    if (ftruncate(trace->fd, original_size) != 0 ||
        lseek(trace->fd, original_size, SEEK_SET) < 0)
      saved = errno ? errno : EIO;
    fail_trace(trace, saved);
    return 0;
  }
  trace->seq = next_seq;
  return 1;
}

int trace_event(TraceWriter *trace, const char *event, const char *data_json) {
  return write_event(trace, event, data_json, NULL, 0, 0, 0, 0, 0);
}

int trace_tool_event(TraceWriter *trace, const char *event, const char *name,
                     int has_result, int ok, long long wall_duration_ms,
                     long long permission_wait_ms, size_t result_bytes) {
  return write_event(trace, event, NULL, name ? name : "unknown", has_result,
                     ok, wall_duration_ms, permission_wait_ms, result_bytes);
}

int trace_terminal_event(TraceWriter *trace, const char *event,
                         const char *data_json) {
  if (!trace || trace->state == TRACE_STATE_DISABLED)
    return 1;
  if (trace->state != TRACE_STATE_ACTIVE || !event ||
      strcmp(event, "run.finished") != 0)
    return 0;
  if (!write_event(trace, event, data_json, NULL, 0, 0, 0, 0, 0))
    return 0;
  trace->terminal_written = 1;
  trace->state = TRACE_STATE_TERMINAL;
  return 1;
}

static int finish_error(TraceWriter *trace, char *error, size_t error_size) {
  int saved = trace && trace->failure_errno ? trace->failure_errno : EIO;
  if (error && error_size)
    snprintf(error, error_size, "requested trace could not be finalized: %s",
             strerror(saved));
  return 0;
}

int trace_finish(TraceWriter *trace, char *error, size_t error_size) {
  if (!trace || trace->state == TRACE_STATE_DISABLED)
    return 1;
  if (trace->state == TRACE_STATE_PUBLISHED)
    return 1;
  if (trace->state == TRACE_STATE_FAILED ||
      trace->state == TRACE_STATE_ABORTED) {
    release_handles(trace);
    return finish_error(trace, error, error_size);
  }
  if (trace->state != TRACE_STATE_TERMINAL || !trace->terminal_written) {
    fail_trace(trace, EINVAL);
    release_handles(trace);
    return finish_error(trace, error, error_size);
  }

  if (!owns_partial(trace))
    fail_trace(trace, ESTALE);
  if (trace->state != TRACE_STATE_FAILED && fsync(trace->fd) != 0)
    fail_trace(trace, errno);
  if (close(trace->fd) != 0 && trace->state != TRACE_STATE_FAILED)
    fail_trace(trace, errno);
  trace->fd = -1;

  if (trace->state != TRACE_STATE_FAILED && !owns_partial(trace))
    fail_trace(trace, ESTALE);
  if (trace->state != TRACE_STATE_FAILED &&
      renameat(trace->dir_fd, trace->partial_name, trace->dir_fd,
               trace->target_name) != 0)
    fail_trace(trace, errno);
  if (trace->state != TRACE_STATE_FAILED &&
      !owns_trace_leaf(trace, trace->target_name, 1)) {
    if (owns_trace_leaf(trace, trace->target_name, 0))
      (void)unlinkat(trace->dir_fd, trace->target_name, 0);
    fail_trace(trace, ESTALE);
  }

  if (trace->state == TRACE_STATE_FAILED) {
    release_handles(trace);
    return finish_error(trace, error, error_size);
  }

  /* renameat() is the publication point. A directory fsync improves crash
   * durability, but after a successful rename the target is already visible
   * and cannot be truthfully reported as unpublished. */
  trace->state = TRACE_STATE_PUBLISHED;
  (void)fsync(trace->dir_fd);
  release_handles(trace);
  return 1;
}

void trace_close(TraceWriter *trace) {
  if (!trace || trace->state == TRACE_STATE_DISABLED ||
      trace->state == TRACE_STATE_PUBLISHED ||
      trace->state == TRACE_STATE_ABORTED)
    return;
  if (!trace->failure_errno)
    trace->failure_errno = ECANCELED;
  trace->state = TRACE_STATE_ABORTED;
  release_handles(trace);
}

int trace_failed(const TraceWriter *trace) {
  return trace && (trace->state == TRACE_STATE_FAILED ||
                   trace->state == TRACE_STATE_ABORTED);
}

int trace_is_active(const TraceWriter *trace) {
  return trace && (trace->state == TRACE_STATE_ACTIVE ||
                   trace->state == TRACE_STATE_TERMINAL);
}

void trace_accumulate_tool_breakdown(int is_subagent,
                                     long long wall_duration_ms,
                                     long long permission_wait_ms,
                                     long long *tool_ms,
                                     long long *permission_ms,
                                     long long *subagent_ms) {
  if (wall_duration_ms < 0)
    wall_duration_ms = 0;
  if (permission_wait_ms < 0)
    permission_wait_ms = 0;
  if (permission_wait_ms > wall_duration_ms)
    permission_wait_ms = wall_duration_ms;
  if (permission_ms)
    *permission_ms += permission_wait_ms;
  long long execution_ms = wall_duration_ms - permission_wait_ms;
  if (is_subagent) {
    if (subagent_ms)
      *subagent_ms += execution_ms;
  } else if (tool_ms) {
    *tool_ms += execution_ms;
  }
}
