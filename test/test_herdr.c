#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "herdr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void settle(const char *session, const char *state) {
  for (int i = 0; i < 40; i++) {
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
    herdr_tick(session, state);
  }
}

int main(void) {
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
  herdr_tick("session", "working");
  assert(access(output, F_OK));
  setenv("HERDR_ENV", "0", 1);
  herdr_tick("session", "working");
  assert(access(output, F_OK));
  setenv("HERDR_ENV", "1", 1);
  unsetenv("HERDR_SOCKET_PATH");
  herdr_tick("session", "working");
  assert(access(output, F_OK));
  setenv("HERDR_SOCKET_PATH", "fake-unused-socket", 1);
  herdr_tick("session;literal", "invalid");
  assert(access(output, F_OK));
  settle("session;literal", "working");
  settle("session;literal", "idle");
  settle("other", "blocked");
  herdr_shutdown();
  struct timespec pause = {0, 100000000};
  nanosleep(&pause, NULL);
  f = fopen(output, "r");
  assert(f);
  char actual[4096] = {0};
  size_t n = fread(actual, 1, sizeof(actual) - 1, f);
  assert(n && !ferror(f));
  fclose(f);
  const char *expected =
    "BEGIN\npane\nreport-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n--state\nworking\n--agent-session-id\nsession;literal\n"
    "BEGIN\npane\nreport-agent\npane;literal\n--source\ncapstan\n--agent\ncapstan\n--state\nidle\n--agent-session-id\nsession;literal\n"
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
  settle("retry", "idle");
  f = fopen(output, "r");
  assert(f);
  memset(actual, 0, sizeof(actual));
  assert(fread(actual, 1, sizeof(actual) - 1, f));
  fclose(f);
  assert(!strcmp(actual, "attempt\n"));
  struct timespec retry_pause = {1, 100000000};
  nanosleep(&retry_pause, NULL);
  settle("retry", "idle");
  settle("retry", "idle");
  f = fopen(output, "r");
  assert(f);
  memset(actual, 0, sizeof(actual));
  assert(fread(actual, 1, sizeof(actual) - 1, f));
  fclose(f);
  assert(!strcmp(actual, "attempt\nattempt\n"));
  herdr_shutdown();
  nanosleep(&pause, NULL);
  char failed[300];
  snprintf(failed, sizeof(failed), "%s.failed", output);
  unlink(failed);
  unlink(output);
  unlink(bin);
  rmdir(dir);
  puts("herdr adapter: passed");
  return 0;
}
