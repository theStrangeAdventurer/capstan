#include "background_work.h"
#include "munit.h"
#include <stdlib.h>
#include <string.h>
static MunitResult registry(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  background_work_shutdown();
  char a[PROCESS_ID_SIZE], b[PROCESS_ID_SIZE], group[PROCESS_ID_SIZE];
  munit_assert_true(background_work_register("a", "subagent", "worker", ".", a));
  munit_assert_true(background_work_register("b", "subagent", "other", ".", b));
  munit_assert_true(background_work_register("a", "subagent_group", "group", ".", group));
  BackgroundSnapshot s;
  munit_assert_size(background_work_count(), ==, 3);
  munit_assert_true(background_work_get(a, &s));
  munit_assert_int(s.pid, ==, 0);
  munit_assert_string_equal(background_work_status(&s), "queued");
  munit_assert_true(background_work_update(a, "running", "\033[31mAPI_KEY=fictional-secret\033[0m\nvisible", 0));
  char *out = background_work_output(a, 0);
  munit_assert_not_null(out);
  munit_assert_null(strstr(out, "fictional-secret"));
  munit_assert_null(strchr(out, 27));
  munit_assert_not_null(strstr(out, "visible")); free(out);
  munit_assert_size(background_work_cancel_owner("a"), ==, 1);
  munit_assert_true(background_work_cancelled(a));
  munit_assert_true(background_work_cancelled(group));
  munit_assert_false(background_work_cancelled(b));
  munit_assert_true(background_work_get(a, &s));
  munit_assert_true(s.running); /* Stop only queues cancellation, no callbacks. */
  munit_assert_size(background_work_cancel_owner("a"), ==, 0);
  munit_assert_true(background_work_update(a, "cancelled", "cancelled", 0));
  munit_assert_false(background_work_completion("a", &s));
  munit_assert_true(background_work_update(group, "cancelled", "{\"ok\":false}", 0));
  munit_assert_false(background_work_completion("b", &s));
  munit_assert_true(background_work_completion("a", &s));
  munit_assert_string_equal(s.id, group);
  munit_assert_false(background_work_update(group, "completed", "changed", 1));
  out = background_work_output(group, 0);
  munit_assert_string_equal(out, "{\"ok\":false}"); free(out);
  munit_assert_false(background_work_completion("a", &s));
  munit_assert_false(background_work_update(a, "running", NULL, 0));
  munit_assert_false(background_work_update(b, "bad", NULL, 0));
  background_work_shutdown();
  return MUNIT_OK;
}
static MunitResult bounds(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  background_work_shutdown();
  char id[PROCESS_ID_SIZE], first[PROCESS_ID_SIZE];
  for (int i = 0; i < 128; i++) {
    munit_assert_true(background_work_register("owner", "subagent", "task", ".", id));
    if (!i) strcpy(first, id);
  }
  munit_assert_false(background_work_register("owner", "subagent", "full", ".", id));
  char *large = malloc(100001); munit_assert_not_null(large);
  memset(large, 'x', 100000); large[100000] = 0;
  munit_assert_true(background_work_update(first, "running", "previous", 0));
  munit_assert_false(background_work_update(first, "completed", large, 1));
  char *out = background_work_output(first, 0);
  munit_assert_string_equal(out, "previous"); free(out);
  BackgroundSnapshot s;
  munit_assert_true(background_work_get(first, &s));
  munit_assert_true(s.running); munit_assert_false(s.truncated);
  large[65536] = 0;
  munit_assert_true(background_work_update(first, "completed", large, 1)); free(large);
  out = background_work_output(first, 0);
  munit_assert_size(strlen(out), ==, 65536); free(out);
  munit_assert_false(background_work_register("owner", "subagent", "replacement", ".", id));
  munit_assert_true(background_work_get(first, &s));
  munit_assert_false(s.running);
  munit_assert_size(background_work_count(), ==, 128);
  background_work_shutdown();
  return MUNIT_OK;
}
static MunitResult release_records(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  background_work_shutdown();
  char ids[128][PROCESS_ID_SIZE], stale[PROCESS_ID_SIZE] = "unknown";
  BackgroundSnapshot s;
  munit_assert_false(background_work_release(NULL));
  for (int cycle = 0; cycle < 10; cycle++) {
    for (int i = 0; i < 128; i++)
      munit_assert_true(background_work_register_ex("owner", "subagent_group", "muted", ".", 0, ids[i]));
    munit_assert_false(background_work_get(stale, &s));
    munit_assert_false(background_work_update(stale, "completed", NULL, 1));
    munit_assert_false(background_work_stop(stale));
    munit_assert_false(background_work_release(stale));
    munit_assert_false(background_work_release(ids[0]));
    munit_assert_true(background_work_stop(ids[0]));
    munit_assert_false(background_work_release(ids[0]));
    for (int i = 0; i < 128; i++) {
      munit_assert_true(background_work_update(ids[i], "completed", "retained output", 1));
      munit_assert_true(background_work_get(ids[i], &s));
      munit_assert_false(s.running);
      munit_assert_true(background_work_at((size_t)i, &s));
      munit_assert_string_equal(s.id, ids[i]);
    }
    munit_assert_size(background_work_count(), ==, 128);
    munit_assert_false(background_work_completion("owner", &s));
    char extra[PROCESS_ID_SIZE];
    munit_assert_false(background_work_register("owner", "subagent", "full", ".", extra));
    for (int i = 0; i < 128; i++) {
      char *out = background_work_output(ids[i], 0);
      munit_assert_string_equal(out, "retained output"); free(out);
      munit_assert_true(background_work_release(ids[i]));
      munit_assert_false(background_work_get(ids[i], &s));
      munit_assert_false(background_work_release(ids[i]));
      munit_assert_size(background_work_count(), ==, (size_t)(127 - i));
    }
    strcpy(stale, ids[0]);
  }
  background_work_shutdown();
  return MUNIT_OK;
}
static MunitTest tests[] = {
  {"/release", release_records, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/registry", registry, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/bounds", bounds, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite background_work_suite = {"/background_work", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
#ifdef BACKGROUND_WORK_TEST_MAIN
int main(int argc, char **argv) { return munit_suite_main(&background_work_suite, NULL, argc, argv); }
#endif
