#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
/* Inspect the actual in-flight child/cache instead of guessing scheduler delays. */
#include "../src/herdr.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void pause_tick(void) {
  struct timespec pause = {0, 5000000};
  nanosleep(&pause, NULL);
}

static void drain(void) {
  double deadline = monotonic_now() + 10;
  while (child) {
    assert(monotonic_now() < deadline);
    herdr_tick(NULL, NULL, NULL);
    pause_tick();
  }
}

static void settle_resume(const char *session, const char *state,
                           const char *const *resume) {
  double deadline = monotonic_now() + 10;
  do {
    assert(monotonic_now() < deadline);
    herdr_tick(session, state, resume);
    drain();
    if (retry_after > monotonic_now()) pause_tick();
  } while (!last_state || strcmp(last_state, state) ||
           !last_session || strcmp(last_session, session));
  /* An unchanged render must not create another child. */
  herdr_tick(session, state, resume);
  assert(!child);
}

static void settle(const char *session, const char *state) {
  settle_resume(session, state, NULL);
}

static void test_resume_limits(void) {
  size_t count, size;
  char huge[8193];
  memset(huge, 'a', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = 0;
  const char *bad[][4] = {
    {"/bin/capstan", NULL}, {"cap stan", NULL},
    {"capstan", "apostrophe'", NULL}, {"capstan", "line\nbreak", NULL},
    {"capstan", "\xc2\x85", NULL}, {"capstan", huge, NULL}
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char *key = resume_key(bad[i], &count, &size);
    assert(key && !count && size == 1 && !*key);
    free(key);
  }
  const char *many[66];
  for (int i = 0; i < 65; i++) many[i] = "capstan";
  many[65] = NULL;
  char *key = resume_key(many, &count, &size);
  assert(key && !count);
  free(key);
  many[64] = NULL;
  key = resume_key(many, &count, &size);
  assert(key && count == 64);
  free(key);
  huge[8192 - sizeof("capstan") - 1] = 0;
  const char *limit[] = {"capstan", huge, NULL};
  key = resume_key(limit, &count, &size);
  assert(key && count == 2 && size == 8193);
  free(key);
}

int main(void) {
  test_resume_limits();
  char dir[] = "build/herdr-test.XXXXXX";
  assert(mkdtemp(dir));
  char bin[256], output[256];
  snprintf(bin, sizeof(bin), "%s/fake herdr", dir);
  snprintf(output, sizeof(output), "%s/argv", dir);
  FILE *f = fopen(bin, "w");
  assert(f);
  fputs("#!/bin/sh\nprintf '%s\\n' BEGIN \"$@\" >> \"$CAPSTAN_HERDR_TEST_OUTPUT\"\necho unwanted-output\necho unwanted-error >&2\n", f);
  assert(!fclose(f));
  assert(!chmod(bin, 0700));
  assert(!setenv("CAPSTAN_HERDR_TEST_OUTPUT", output, 1));
  assert(!setenv("HERDR_BIN_PATH", bin, 1));
  assert(!setenv("HERDR_PANE_ID", "pane;literal", 1));
  assert(!setenv("HERDR_SOCKET_PATH", "fake-unused-socket", 1));
  unsetenv("HERDR_ENV");
  herdr_tick("session", "working", NULL);
  assert(access(output, F_OK));
  setenv("HERDR_ENV", "0", 1);
  herdr_tick("session", "working", NULL);
  assert(access(output, F_OK));
  setenv("HERDR_ENV", "1", 1);
  unsetenv("HERDR_SOCKET_PATH");
  herdr_tick("session", "working", NULL);
  assert(access(output, F_OK));
  setenv("HERDR_SOCKET_PATH", "fake-unused-socket", 1);
  herdr_tick("session;literal", "invalid", NULL);
  assert(access(output, F_OK));
  const char *resume[] = {"capstan", "--session-id", "session;literal",
                          "--workdir", "/space dir/Проверка", NULL};
  settle_resume("session;literal", "working", resume);
  settle_resume("session;literal", "idle", resume);
  /* Same state/session, different model must update the restore command. */
  const char *changed[] = {"capstan", "--session-id", "session;literal",
                           "--model", "new-model", NULL};
  settle_resume("session;literal", "idle", changed);
  const char *invalid[] = {"capstan", "--workdir", "/can't-restore", NULL};
  settle_resume("other", "blocked", invalid);
  herdr_shutdown();

  f = fopen(output, "r");
  assert(f);
  char actual[4096] = {0};
  size_t n = fread(actual, 1, sizeof(actual) - 1, f);
  assert(n && !ferror(f));
  fclose(f);
  const char *expected =
    "BEGIN\npane\nreport-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n--state\nworking\n--agent-session-id\nsession;literal\n--\ncapstan\n--session-id\nsession;literal\n--workdir\n/space dir/Проверка\n"
    "BEGIN\npane\nreport-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n--state\nidle\n--agent-session-id\nsession;literal\n--\ncapstan\n--session-id\nsession;literal\n--workdir\n/space dir/Проверка\n"
    "BEGIN\npane\nreport-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n--state\nidle\n--agent-session-id\nsession;literal\n--\ncapstan\n--session-id\nsession;literal\n--model\nnew-model\n"
    "BEGIN\npane\nreport-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n--state\nblocked\n--agent-session-id\nother\n"
    "BEGIN\npane\nrelease-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n";
  assert(!strcmp(actual, expected));
  unlink(output);

  /* A failed idle report must retry unchanged state, but not every frame. */
  f = fopen(bin, "w");
  assert(f);
  fputs("#!/bin/sh\nprintf '%s\\n' attempt >> \"$CAPSTAN_HERDR_TEST_OUTPUT\"\n"
        "if [ ! -f \"$CAPSTAN_HERDR_TEST_OUTPUT.failed\" ]; then\n"
        "  touch \"$CAPSTAN_HERDR_TEST_OUTPUT.failed\"; exit 1\nfi\n", f);
  assert(!fclose(f));
  herdr_tick("retry", "idle", NULL);
  drain();
  assert(retry_after > monotonic_now());
  herdr_tick("retry", "idle", NULL);
  assert(!child);
  f = fopen(output, "r");
  assert(f);
  memset(actual, 0, sizeof(actual));
  assert(fread(actual, 1, sizeof(actual) - 1, f));
  fclose(f);
  assert(!strcmp(actual, "attempt\n"));
  settle("retry", "idle");
  settle("retry", "idle");
  f = fopen(output, "r");
  assert(f);
  memset(actual, 0, sizeof(actual));
  assert(fread(actual, 1, sizeof(actual) - 1, f));
  fclose(f);
  assert(!strcmp(actual, "attempt\nattempt\n"));
  herdr_shutdown();
  char failed[300];
  snprintf(failed, sizeof(failed), "%s.failed", output);
  unlink(failed);
  unlink(output);
  unlink(bin);
  rmdir(dir);
  puts("herdr adapter: passed");
  return 0;
}
