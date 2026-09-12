#include "munit.h"
#include "process_manager.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void pause_poll(void) {
  struct timespec ts = {0, 10000000};
  nanosleep(&ts, NULL);
  process_manager_poll();
}
static ProcessSnapshot finish(const char *id) {
  ProcessSnapshot s;
  long long deadline = process_manager_now_ms() + 4500;
  do {
    pause_poll();
    munit_assert_true(process_manager_get(id, &s));
    munit_assert_true(process_manager_now_ms() < deadline);
  } while (s.running);
  return s;
}
static void *setup(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  process_manager_shutdown();
  return NULL;
}
static void teardown(void *data) { (void)data; process_manager_shutdown(); }
static MunitResult test_running_stop(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  process_manager_set_owner("manager-test");
  munit_assert_true(process_manager_start("sleep 30", NULL, ".", 0, 1024, 1024, id));
  ProcessSnapshot s;
  munit_assert_true(process_manager_get(id, &s));
  munit_assert_true(s.running);
  munit_assert_string_equal(s.owner, "manager-test");
  munit_assert_string_equal(process_manager_owner(), "manager-test");
  munit_assert_size(process_manager_count(), ==, 1);
  munit_assert_true(process_manager_at(0, &s));
  munit_assert_false(process_manager_at(1, &s));
  long long start = process_manager_now_ms();
  process_manager_poll();
  munit_assert_true(process_manager_now_ms() - start < 100);
  munit_assert_true(process_manager_stop(id));
  munit_assert_true(process_manager_stop(id));
  s = finish(id);
  munit_assert_false(s.timed_out);
  munit_assert_false(process_manager_stop(id));
  return MUNIT_OK;
}
static MunitResult test_output(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start("printf 'API_'; sleep 1; printf '\033[31mKEY=fictional-test-value\033[0m\\n'; printf error >&2; exit 7", NULL, ".", 3, 1024, 1024, id));
  char *text = process_manager_output(id, 0);
  munit_assert_null(strstr(text, "API_")); free(text);
  ProcessSnapshot s = finish(id);
  munit_assert_int(s.exit_code, ==, 7);
  text = process_manager_output(id, 0);
  munit_assert_string_equal(text, "API_KEY=[REDACTED]\n"); free(text);
  text = process_manager_raw_output(id, 0);
  munit_assert_not_null(strchr(text, 27)); free(text);
  text = process_manager_output(id, 1);
  munit_assert_string_equal(text, "error"); free(text);
  return MUNIT_OK;
}
static MunitResult test_timeout_descendants(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start("(trap '' TERM; sleep 30) & exit 0", NULL, ".", 1, 1024, 1024, id));
  ProcessSnapshot s = finish(id);
  munit_assert_true(s.timed_out);
  /* Leader status is retained, even though descendants required escalation. */
  munit_assert_int(s.exit_code, ==, 0);
  munit_assert_true(s.finished_ms - s.started_ms < 4000);
  return MUNIT_OK;
}
static MunitResult test_background_output(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start("(sleep 1; printf background) & exit 0", NULL, ".", 3, 1024, 1024, id));
  ProcessSnapshot s = finish(id);
  munit_assert_false(s.timed_out);
  char *text = process_manager_output(id, 0);
  munit_assert_string_equal(text, "background"); free(text);
  return MUNIT_OK;
}
static MunitResult test_overflow_stale(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char first[PROCESS_ID_SIZE], id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start("printf abcdef", NULL, ".", 2, 3, 0, first));
  ProcessSnapshot s = finish(first);
  munit_assert_true(s.truncated);
  char *text = process_manager_raw_output(first, 0);
  munit_assert_string_equal(text, "abc"); free(text);
  text = process_manager_output(first, 0);
  munit_assert_string_equal(text, "abc\n[output truncated]"); free(text);
  for (int i = 0; i < 128; i++) {
    munit_assert_true(process_manager_start("exit 0", NULL, ".", 2, 0, 0, id));
    finish(id);
  }
  munit_assert_size(process_manager_count(), ==, 128);
  munit_assert_false(process_manager_get(first, &s));
  munit_assert_false(process_manager_stop(first));
  munit_assert_null(process_manager_output(first, 0));
  process_manager_shutdown();
  munit_assert_true(process_manager_start("true", NULL, ".", 2, 0, 0, id));
  munit_assert_string_not_equal(first, id);
  finish(id);
  return MUNIT_OK;
}
static MunitResult test_closed_fds_and_error(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start("exec 1>&- 2>&-; sleep 1; exit 9", NULL, ".", 3, 1024, 1024, id));
  ProcessSnapshot s = finish(id);
  munit_assert_int(s.exit_code, ==, 9);
  char *argv[] = {"/nonexistent-capstan-test-executable", NULL};
  munit_assert_true(process_manager_start(NULL, argv, ".", 2, 0, 0, id));
  s = finish(id); munit_assert_int(s.exit_code, ==, 127);
  munit_assert_false(process_manager_start(NULL, NULL, ".", 2, 0, 0, id));
  return MUNIT_OK;
}
static MunitResult test_adopt_echild(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  int p[2]; munit_assert_int(pipe(p), ==, 0);
  pid_t pid = fork(); munit_assert_true(pid >= 0);
  if (!pid) { setpgid(0, 0); close(p[0]); close(p[1]); _exit(12); }
  setpgid(pid, pid); close(p[1]);
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_adopt(pid, "test", "child", ".", p[0], -1, 16, 0, 0, id));
  munit_assert_true(fcntl(p[0], F_GETFL) & O_NONBLOCK);
  munit_assert_true(fcntl(p[0], F_GETFD) & FD_CLOEXEC);
  /* Deliberately violate single wait-owner contract: never report false success. */
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
  ProcessSnapshot s = finish(id);
  munit_assert_int(s.exit_code, ==, -1);
  munit_assert_int(fcntl(p[0], F_GETFD), ==, -1);
  return MUNIT_OK;
}
static MunitResult test_live_output(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start(
    "printf 'ready\\nAPI_\\033[31mKEY=fictional-value\\033[0m\\npartial'; sleep 30",
    NULL, ".", 0, 1024, 1024, id));
  long long end = process_manager_now_ms() + 2000;
  for (;;) {
    pause_poll();
    char *raw = process_manager_raw_output(id, 0);
    int ready = strstr(raw, "partial") != NULL;
    free(raw);
    if (ready) break;
    munit_assert_true(process_manager_now_ms() < end);
  }
  ProcessSnapshot s;
  munit_assert_true(process_manager_get(id, &s));
  munit_assert_true(s.running);
  char *text = process_manager_output(id, 0);
  munit_assert_string_equal(text, "ready\nAPI_KEY=[REDACTED]\n");
  free(text);
  process_manager_stop(id); finish(id);
  text = process_manager_output(id, 0);
  munit_assert_string_equal(text, "ready\nAPI_KEY=[REDACTED]\npartial"); free(text);
  return MUNIT_OK;
}
static MunitResult test_metadata(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char label[2048];
  memset(label, 'x', sizeof(label));
  memcpy(label, "API_\033[31mKEY=\"", 14);
  strcpy(label + 1900, "\" visible");
  pid_t pid = fork(); munit_assert_true(pid >= 0);
  if (!pid) { setpgid(0, 0); sleep(30); _exit(0); }
  setpgid(pid, pid);
  char id[PROCESS_ID_SIZE];
  process_manager_set_owner("API_KEY=owner-value");
  munit_assert_true(process_manager_adopt(pid, "\033[31mtest\033[0m", label,
    "API_KEY=path-value", -1, -1, 0, 0, 0, id));
  ProcessSnapshot s;
  munit_assert_true(process_manager_get(id, &s));
  munit_assert_string_equal(s.kind, "test");
  munit_assert_string_equal(s.label, "API_KEY=\"[REDACTED]\" visible");
  munit_assert_string_equal(s.workdir, "API_KEY=[REDACTED]");
  munit_assert_string_equal(s.owner, "API_KEY=[REDACTED]");
  munit_assert_string_equal(process_manager_owner(), s.owner);
  process_manager_set_owner("");
  process_manager_stop(id); finish(id);
  return MUNIT_OK;
}
static MunitResult test_overflow_redaction(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  munit_assert_true(process_manager_start("printf 'ready\\nAPI_KEY=fictional-overflow-value'; sleep 30",
    NULL, ".", 0, 22, 0, id));
  ProcessSnapshot s;
  long long end = process_manager_now_ms() + 2000;
  do {
    pause_poll(); munit_assert_true(process_manager_get(id, &s));
    munit_assert_true(process_manager_now_ms() < end);
  } while (!s.truncated);
  char *text = process_manager_output(id, 0);
  munit_assert_string_equal(text, "ready\n\n[output truncated]"); free(text);
  process_manager_stop(id); finish(id);
  text = process_manager_output(id, 0);
  munit_assert_not_null(strstr(text, "ready\nAPI_KEY="));
  munit_assert_null(strstr(text, "fictional"));
  munit_assert_not_null(strstr(text, "truncated")); free(text);
  return MUNIT_OK;
}
static MunitResult test_runtime_ids(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char ids[2][PROCESS_ID_SIZE];
  for (int i = 0; i < 2; i++) {
    int p[2]; munit_assert_int(pipe(p), ==, 0);
    pid_t pid = fork(); munit_assert_true(pid >= 0);
    if (!pid) {
      close(p[0]);
      char id[PROCESS_ID_SIZE];
      if (!process_manager_start("true", NULL, ".", 2, 0, 0, id)) _exit(1);
      if (write(p[1], id, sizeof(id)) != sizeof(id)) _exit(2);
      close(p[1]); process_manager_shutdown(); _exit(0);
    }
    close(p[1]);
    size_t used = 0;
    while (used < PROCESS_ID_SIZE) {
      ssize_t n = read(p[0], ids[i] + used, PROCESS_ID_SIZE - used);
      if (n < 0 && errno == EINTR) continue;
      munit_assert_true(n > 0); used += (size_t)n;
    }
    close(p[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    munit_assert_true(WIFEXITED(status)); munit_assert_int(WEXITSTATUS(status), ==, 0);
  }
  munit_assert_string_not_equal(ids[0], ids[1]);
  munit_assert_false(process_manager_stop(ids[0]));
  munit_assert_false(process_manager_stop("process-1"));
  return MUNIT_OK;
}
static MunitResult test_argv_label(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  char id[PROCESS_ID_SIZE];
  char *argv[] = {"/bin/echo", "-y", "@playwright/mcp@latest", "--headless",
    "two words", "", "a\"b\\c", "line\nbreak", NULL};
  munit_assert_true(process_manager_start(NULL, argv, ".", 2, 0, 0, id));
  ProcessSnapshot s = finish(id);
  munit_assert_string_equal(s.label,
    "/bin/echo -y @playwright/mcp@latest --headless \"two words\" \"\" \"a\\\"b\\\\c\" \"line\\nbreak\"");
  char long_secret[2048];
  memset(long_secret, 'x', sizeof(long_secret) - 1);
  long_secret[sizeof(long_secret) - 1] = 0;
  char *sensitive[] = {"/bin/echo", "--to\033[31mken", long_secret,
    "--password=fictional value with 'quotes'", "--cookie", "fictional cookie",
    "--headless", NULL};
  munit_assert_true(process_manager_start(NULL, sensitive, ".", 2, 0, 0, id));
  s = finish(id);
  munit_assert_string_equal(s.label,
    "/bin/echo --token [REDACTED] --password=[REDACTED] --cookie [REDACTED] --headless");
  char *long_args[] = {"/bin/echo", long_secret, "--headless", NULL};
  munit_assert_true(process_manager_start(NULL, long_args, ".", 2, 0, 0, id));
  s = finish(id);
  munit_assert_size(strlen(s.label), ==, PROCESS_TEXT_SIZE - 1);
  munit_assert_string_equal(s.label + PROCESS_TEXT_SIZE - 4, "...");
  return MUNIT_OK;
}
#define TEST(name) {"/" #name, test_##name, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL}
static MunitTest tests[] = {
  TEST(running_stop), TEST(output), TEST(timeout_descendants), TEST(background_output),
  TEST(overflow_stale), TEST(closed_fds_and_error), TEST(adopt_echild),
  TEST(live_output), TEST(metadata), TEST(overflow_redaction), TEST(runtime_ids),
  TEST(argv_label),
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite process_manager_suite = {"/process_manager", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
