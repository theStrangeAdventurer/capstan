#include "munit.h"
#include "process_observe.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__)
typedef struct { pid_t root, sibling; int release, ready; } Fixture;
typedef struct { pid_t pid; int detached; } Ready;
static void wait_child(pid_t pid) {
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}
static void await_release(int fd) {
  char byte;
  while (read(fd, &byte, 1) < 0 && errno == EINTR) {}
  close(fd);
}
static void *setup(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  Fixture *f = calloc(1, sizeof(*f));
  munit_assert_not_null(f);
  int gate[2], ready[2];
  munit_assert_int(pipe(gate), ==, 0);
  munit_assert_int(pipe(ready), ==, 0);
  f->root = fork();
  munit_assert_true(f->root >= 0);
  if (!f->root) {
    close(gate[1]); close(ready[0]);
    if (setpgid(0, 0) < 0) _exit(2);
    pid_t children[2];
    for (int i = 0; i < 2; i++) {
      children[i] = fork();
      if (children[i] < 0) _exit(3);
      if (!children[i]) {
        if (i && setpgid(0, 0) < 0) _exit(4);
        Ready r = {getpid(), i};
        if (write(ready[1], &r, sizeof(r)) != sizeof(r)) _exit(5);
        close(ready[1]); await_release(gate[0]); _exit(0);
      }
    }
    close(ready[1]); await_release(gate[0]);
    wait_child(children[0]); wait_child(children[1]); _exit(0);
  }
  f->sibling = fork();
  munit_assert_true(f->sibling >= 0);
  if (!f->sibling) {
    close(gate[1]); close(ready[0]); close(ready[1]);
    await_release(gate[0]); _exit(0);
  }
  close(gate[0]); close(ready[1]);
  f->release = gate[1]; f->ready = ready[0];
  return f;
}
static void teardown(void *data) {
  Fixture *f = data;
  close(f->release); close(f->ready);
  wait_child(f->root); wait_child(f->sibling); free(f);
}
static MunitResult test_ancestry(const MunitParameter params[], void *data) {
  (void)params;
  Fixture *f = data;
  Ready ready[2];
  for (int i = 0; i < 2; i++) {
    struct pollfd p = {f->ready, POLLIN, 0};
    munit_assert_int(poll(&p, 1, 5000), ==, 1);
    munit_assert_int(read(f->ready, &ready[i], sizeof(Ready)), ==, sizeof(Ready));
  }
  ProcessDescendant rows[8];
  size_t n = process_observe_descendants(f->root, f->root, rows, 8);
  munit_assert_size(n, ==, 2);
  for (size_t i = 0; i < n; i++) {
    munit_assert_true(rows[i].pid != f->root && rows[i].pid != f->sibling);
    munit_assert_int(rows[i].ppid, ==, f->root);
    int match = -1;
    for (int j = 0; j < 2; j++) if (ready[j].pid == rows[i].pid) match = j;
    munit_assert_true(match >= 0);
    munit_assert_int(rows[i].in_managed_group, ==, !ready[match].detached);
    munit_assert_int(rows[i].pgid, ==, ready[match].detached ? rows[i].pid : f->root);
    munit_assert_true(rows[i].name[0] != '\0');
  }
  munit_assert_size(process_observe_descendants(f->root, f->root, rows, 1), ==, 1);
  munit_assert_size(process_observe_descendants(f->root, f->root, NULL, 8), ==, 0);
  munit_assert_size(process_observe_descendants(f->root, f->root, rows, 0), ==, 0);
  munit_assert_size(process_observe_descendants(0, f->root, rows, 8), ==, 0);
  munit_assert_size(process_observe_descendants(f->root, getpgrp(), rows, 8), ==, 0);
  /* Empty ancestry is not inferred from process-group membership. */
  munit_assert_size(process_observe_descendants(f->sibling, getpgrp(), rows, 8), ==, 0);
  return MUNIT_OK;
}
static MunitTest tests[] = {
  {"/ancestry", test_ancestry, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
#else
static MunitTest tests[] = {
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
#endif
MunitSuite process_observe_suite = {"/process_observe", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
