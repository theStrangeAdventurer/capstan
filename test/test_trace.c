#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "munit.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void cleanup_trace_path(const char *path) {
  char related[1024];
  unlink(path);
  snprintf(related, sizeof(related), "%s.partial", path);
  unlink(related);
  snprintf(related, sizeof(related), "%s.lock", path);
  unlink(related);
}

static MunitResult test_writes_jsonl_and_secures_file(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-test-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_true(trace_open(&trace, path, error, sizeof(error)));
  munit_assert_true(trace_event(&trace, "quoted\"event", "{\"ok\":true}"));
  munit_assert_true(trace_tool_event(&trace, "tool.finished",
                                     "quoted\"tool\nname", 1, 1, 15, 3, 34));
  munit_assert_true(trace_terminal_event(&trace, "run.finished",
                                         "{\"ok\":true}"));
  munit_assert_false(trace_terminal_event(&trace, "run.failed", "{}"));
  munit_assert_int(access(path, F_OK), ==, -1);
  munit_assert_int(access(trace.partial_path, F_OK), ==, 0);
  munit_assert_true(trace_finish(&trace, error, sizeof(error)));
  munit_assert_true(trace_finish(&trace, error, sizeof(error)));
  munit_assert_false(trace_failed(&trace));
  munit_assert_int(trace.state, ==, TRACE_STATE_PUBLISHED);

  struct stat st;
  munit_assert_int(stat(path, &st), ==, 0);
  munit_assert_int(st.st_mode & 0777, ==, 0600);
  FILE *file = fopen(path, "rb");
  munit_assert_not_null(file);
  char content[4096] = "";
  munit_assert_size(fread(content, 1, sizeof(content) - 1, file), >, 0);
  fclose(file);
  munit_assert_not_null(strstr(content, "\"schema\":\"capstan.trace.v1\""));
  munit_assert_not_null(strstr(content, "\"event\":\"quoted\\\"event\""));
  munit_assert_not_null(strstr(content, "\"name\":\"quoted\\\"tool\\nname\""));
  munit_assert_not_null(strstr(content, "\"wall_duration_ms\":15"));
  munit_assert_not_null(strstr(content, "\"execution_ms\":12"));
  munit_assert_not_null(strstr(content, "\"permission_wait_ms\":3"));
  munit_assert_not_null(strstr(content, "\"event\":\"run.finished\""));
  munit_assert_null(strstr(content, "\"event\":\"run.failed\""));
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_inactive_writer_is_noop(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  munit_assert_true(trace_event(&trace, "ignored", "{}"));
  munit_assert_true(trace_terminal_event(&trace, "run.finished", "{}"));
  munit_assert_ulong(trace.seq, ==, 0);
  munit_assert_false(trace.terminal_written);
  munit_assert_false(trace_failed(&trace));
  return MUNIT_OK;
}

static MunitResult test_replaces_truncated_invalid_utf8(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-utf8-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  const char invalid_name[] = {'b', 'a', 'd', (char)0xe2, (char)0x82, '\0'};
  munit_assert_true(trace_open(&trace, path, error, sizeof(error)));
  munit_assert_true(trace_tool_event(&trace, "tool.started", invalid_name,
                                     0, 0, 0, 0, 0));
  munit_assert_true(trace_terminal_event(&trace, "run.finished", "{}"));
  munit_assert_true(trace_finish(&trace, error, sizeof(error)));
  FILE *file = fopen(path, "rb");
  munit_assert_not_null(file);
  char content[2048] = "";
  munit_assert_size(fread(content, 1, sizeof(content) - 1, file), >, 0);
  fclose(file);
  munit_assert_not_null(strstr(content, "bad\\ufffd\\ufffd"));
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_concurrent_writer_is_rejected(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-lock-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter parent = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_true(trace_open(&parent, path, error, sizeof(error)));

  pid_t child = fork();
  munit_assert_int(child, >=, 0);
  if (child == 0) {
    TraceWriter other = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
    char child_error[256] = "";
    int opened = trace_open(&other, path, child_error, sizeof(child_error));
    if (opened) trace_close(&other);
    _exit(!opened && strstr(child_error, "already in use") ? 0 : 1);
  }
  int status = 0;
  munit_assert_int(waitpid(child, &status, 0), ==, child);
  munit_assert_true(WIFEXITED(status));
  munit_assert_int(WEXITSTATUS(status), ==, 0);
  munit_assert_int(access(parent.partial_path, F_OK), ==, 0);
  munit_assert_true(trace_terminal_event(&parent, "run.finished", "{}"));
  munit_assert_true(trace_finish(&parent, error, sizeof(error)));
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_rejects_partial_suffix_target_collision(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  char colliding[264];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-suffix-%ld.jsonl",
           (long)getpid());
  snprintf(colliding, sizeof(colliding), "%s.partial", path);
  cleanup_trace_path(path);
  unlink(colliding);

  TraceWriter owner = TRACE_WRITER_INIT;
  TraceWriter other = TRACE_WRITER_INIT;
  char error[256] = "";
  munit_assert_true(trace_open(&owner, path, error, sizeof(error)));
  munit_assert_false(trace_open(&other, colliding, error, sizeof(error)));
  munit_assert_not_null(strstr(error, "reserved .partial suffix"));
  munit_assert_int(access(owner.partial_path, F_OK), ==, 0);
  munit_assert_true(trace_terminal_event(&owner, "run.finished", "{}"));
  munit_assert_true(trace_finish(&owner, error, sizeof(error)));

  cleanup_trace_path(path);
  unlink(colliding);
  return MUNIT_OK;
}

static MunitResult test_rejects_noncanonical_terminal_event(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-terminal-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter trace = TRACE_WRITER_INIT;
  char error[256] = "";
  munit_assert_true(trace_open(&trace, path, error, sizeof(error)));
  munit_assert_false(trace_terminal_event(&trace, "run.failed", "{}"));
  munit_assert_false(trace.terminal_written);
  munit_assert_int(trace.state, ==, TRACE_STATE_ACTIVE);
  munit_assert_true(trace_terminal_event(&trace, "run.finished", "{}"));
  munit_assert_true(trace_finish(&trace, error, sizeof(error)));
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_hardlinked_partial_fails_closed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  char alias[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-hardlink-%ld.jsonl",
           (long)getpid());
  snprintf(alias, sizeof(alias), "/tmp/capstan-trace-hardlink-alias-%ld",
           (long)getpid());
  cleanup_trace_path(path);
  unlink(alias);
  TraceWriter trace = TRACE_WRITER_INIT;
  char error[256] = "";
  munit_assert_true(trace_open(&trace, path, error, sizeof(error)));
  munit_assert_int(link(trace.partial_path, alias), ==, 0);
  munit_assert_false(trace_event(&trace, "must.not.write", "{}"));
  munit_assert_false(trace_finish(&trace, error, sizeof(error)));
  munit_assert_true(trace_failed(&trace));
  munit_assert_int(access(path, F_OK), ==, -1);
  unlink(alias);
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_relative_path_survives_chdir(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char original[1024];
  munit_assert_not_null(getcwd(original, sizeof(original)));
  char directory[] = "/tmp/capstan-trace-cwd-XXXXXX";
  munit_assert_not_null(mkdtemp(directory));
  munit_assert_int(chdir(directory), ==, 0);

  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_true(trace_open(&trace, "trace.jsonl", error, sizeof(error)));
  munit_assert_int(chdir("/"), ==, 0);
  munit_assert_true(trace_terminal_event(&trace, "run.finished", "{}"));
  munit_assert_true(trace_finish(&trace, error, sizeof(error)));

  char path[1024];
  snprintf(path, sizeof(path), "%s/trace.jsonl", directory);
  munit_assert_int(access(path, F_OK), ==, 0);
  cleanup_trace_path(path);
  munit_assert_int(chdir(original), ==, 0);
  munit_assert_int(rmdir(directory), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_finish_without_terminal_stays_failed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-no-terminal-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_true(trace_open(&trace, path, error, sizeof(error)));
  munit_assert_false(trace_finish(&trace, error, sizeof(error)));
  munit_assert_false(trace_finish(&trace, error, sizeof(error)));
  munit_assert_true(trace_failed(&trace));
  munit_assert_int(access(path, F_OK), ==, -1);
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_close_then_finish_stays_failed(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-abort-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_true(trace_open(&trace, path, error, sizeof(error)));
  trace_close(&trace);
  munit_assert_false(trace_finish(&trace, error, sizeof(error)));
  munit_assert_true(trace_failed(&trace));
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_rejects_target_and_partial_symlinks(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  char partial[264];
  char victim[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-link-%ld.jsonl",
           (long)getpid());
  snprintf(partial, sizeof(partial), "%s.partial", path);
  snprintf(victim, sizeof(victim), "/tmp/capstan-trace-victim-%ld",
           (long)getpid());
  cleanup_trace_path(path);
  unlink(victim);
  FILE *file = fopen(victim, "w");
  munit_assert_not_null(file);
  fputs("preserve-me", file);
  fclose(file);

  munit_assert_int(symlink(victim, path), ==, 0);
  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_false(trace_open(&trace, path, error, sizeof(error)));
  unlink(path);
  munit_assert_int(symlink(victim, partial), ==, 0);
  munit_assert_false(trace_open(&trace, path, error, sizeof(error)));

  file = fopen(victim, "r");
  munit_assert_not_null(file);
  char content[32] = "";
  munit_assert_size(fread(content, 1, sizeof(content) - 1, file), >, 0);
  fclose(file);
  munit_assert_string_equal(content, "preserve-me");
  cleanup_trace_path(path);
  unlink(victim);
  return MUNIT_OK;
}

static MunitResult test_failed_write_preserves_partial_and_blocks_reuse(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-write-fail-%ld.jsonl",
           (long)getpid());
  cleanup_trace_path(path);
  TraceWriter first = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_true(trace_open(&first, path, error, sizeof(error)));
  munit_assert_int(close(first.fd), ==, 0);
  munit_assert_false(trace_event(&first, "cannot.write", "{}"));
  munit_assert_false(trace_finish(&first, error, sizeof(error)));
  munit_assert_int(first.fd, ==, -1);
  munit_assert_int(first.dir_fd, ==, -1);
  munit_assert_int(access(first.partial_path, F_OK), ==, 0);

  TraceWriter second = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  error[0] = '\0';
  munit_assert_false(trace_open(&second, path, error, sizeof(error)));
  munit_assert_not_null(strstr(error, "stale partial"));
  cleanup_trace_path(path);
  return MUNIT_OK;
}

static MunitResult test_rejects_reopen_of_active_writer(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char first_path[256];
  char second_path[256];
  snprintf(first_path, sizeof(first_path),
           "/tmp/capstan-trace-reopen-a-%ld.jsonl", (long)getpid());
  snprintf(second_path, sizeof(second_path),
           "/tmp/capstan-trace-reopen-b-%ld.jsonl", (long)getpid());
  cleanup_trace_path(first_path);
  cleanup_trace_path(second_path);

  TraceWriter trace = TRACE_WRITER_INIT;
  char error[256] = "";
  munit_assert_true(
      trace_open(&trace, first_path, error, sizeof(error)));
  munit_assert_false(
      trace_open(&trace, second_path, error, sizeof(error)));
  munit_assert_not_null(strstr(error, "already active"));
  munit_assert_int(access(trace.partial_path, F_OK), ==, 0);
  munit_assert_true(trace_terminal_event(&trace, "run.finished", "{}"));
  munit_assert_true(trace_finish(&trace, error, sizeof(error)));
  munit_assert_int(access(first_path, F_OK), ==, 0);
  munit_assert_int(access(second_path, F_OK), ==, -1);

  cleanup_trace_path(first_path);
  cleanup_trace_path(second_path);
  return MUNIT_OK;
}

static MunitResult test_invalid_partial_preserves_published_target(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char path[256];
  char partial[264];
  char victim[256];
  snprintf(path, sizeof(path), "/tmp/capstan-trace-preserve-%ld.jsonl",
           (long)getpid());
  snprintf(partial, sizeof(partial), "%s.partial", path);
  snprintf(victim, sizeof(victim), "/tmp/capstan-trace-preserve-victim-%ld",
           (long)getpid());
  cleanup_trace_path(path);
  unlink(victim);
  FILE *file = fopen(path, "w");
  munit_assert_not_null(file);
  fputs("published", file);
  fclose(file);
  file = fopen(victim, "w");
  munit_assert_not_null(file);
  fputs("victim", file);
  fclose(file);
  munit_assert_int(symlink(victim, partial), ==, 0);

  TraceWriter trace = {.fd = -1, .dir_fd = -1, .lock_fd = -1};
  char error[256] = "";
  munit_assert_false(trace_open(&trace, path, error, sizeof(error)));
  file = fopen(path, "r");
  munit_assert_not_null(file);
  char content[32] = "";
  munit_assert_size(fread(content, 1, sizeof(content) - 1, file), >, 0);
  fclose(file);
  munit_assert_string_equal(content, "published");

  cleanup_trace_path(path);
  unlink(victim);
  return MUNIT_OK;
}

static MunitResult test_rejects_unsafe_shared_trace_directory(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char directory[] = "/tmp/capstan-trace-dir-XXXXXX";
  munit_assert_not_null(mkdtemp(directory));
  char path[1024];
  snprintf(path, sizeof(path), "%s/trace.jsonl", directory);
  char error[256] = "";

  TraceWriter private_trace = TRACE_WRITER_INIT;
  munit_assert_true(
      trace_open(&private_trace, path, error, sizeof(error)));
  munit_assert_true(
      trace_terminal_event(&private_trace, "run.finished", "{}"));
  munit_assert_true(trace_finish(&private_trace, error, sizeof(error)));

  munit_assert_int(chmod(directory, 0770), ==, 0);
  TraceWriter group_writable = TRACE_WRITER_INIT;
  munit_assert_false(
      trace_open(&group_writable, path, error, sizeof(error)));
  munit_assert_not_null(strstr(error, "without sticky bit"));
  munit_assert_int(access(path, F_OK), ==, 0);

  munit_assert_int(chmod(directory, 0777), ==, 0);
  TraceWriter world_writable = TRACE_WRITER_INIT;
  munit_assert_false(
      trace_open(&world_writable, path, error, sizeof(error)));
  munit_assert_not_null(strstr(error, "without sticky bit"));

  munit_assert_int(chmod(directory, 01777), ==, 0);
  TraceWriter sticky = TRACE_WRITER_INIT;
  munit_assert_true(trace_open(&sticky, path, error, sizeof(error)));
  munit_assert_true(trace_terminal_event(&sticky, "run.finished", "{}"));
  munit_assert_true(trace_finish(&sticky, error, sizeof(error)));

  cleanup_trace_path(path);
  munit_assert_int(chmod(directory, 0700), ==, 0);
  munit_assert_int(rmdir(directory), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_subagent_permission_time_is_separate(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  long long tool_ms = 7;
  long long permission_ms = 11;
  long long subagent_ms = 13;

  trace_accumulate_tool_breakdown(1, 100, 25, &tool_ms, &permission_ms,
                                  &subagent_ms);
  munit_assert_int64(tool_ms, ==, 7);
  munit_assert_int64(permission_ms, ==, 36);
  munit_assert_int64(subagent_ms, ==, 88);

  trace_accumulate_tool_breakdown(0, 20, 5, &tool_ms, &permission_ms,
                                  &subagent_ms);
  munit_assert_int64(tool_ms, ==, 22);
  munit_assert_int64(permission_ms, ==, 41);
  munit_assert_int64(subagent_ms, ==, 88);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/writes_jsonl_and_secures_file", test_writes_jsonl_and_secures_file,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/inactive_writer_is_noop", test_inactive_writer_is_noop, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/replaces_truncated_invalid_utf8",
     test_replaces_truncated_invalid_utf8, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/concurrent_writer_is_rejected", test_concurrent_writer_is_rejected,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_partial_suffix_target_collision",
     test_rejects_partial_suffix_target_collision, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_noncanonical_terminal_event",
     test_rejects_noncanonical_terminal_event, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/hardlinked_partial_fails_closed",
     test_hardlinked_partial_fails_closed, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/relative_path_survives_chdir", test_relative_path_survives_chdir,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/finish_without_terminal_stays_failed",
     test_finish_without_terminal_stays_failed, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/close_then_finish_stays_failed", test_close_then_finish_stays_failed,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_target_and_partial_symlinks",
     test_rejects_target_and_partial_symlinks, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/failed_write_preserves_partial_and_blocks_reuse",
     test_failed_write_preserves_partial_and_blocks_reuse, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_reopen_of_active_writer",
     test_rejects_reopen_of_active_writer, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/invalid_partial_preserves_published_target",
     test_invalid_partial_preserves_published_target, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_unsafe_shared_trace_directory",
     test_rejects_unsafe_shared_trace_directory, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/subagent_permission_time_is_separate",
     test_subagent_permission_time_is_separate, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite trace_suite = {"/trace", tests, NULL, 1,
                          MUNIT_SUITE_OPTION_NONE};
