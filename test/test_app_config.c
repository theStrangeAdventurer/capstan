#include "app_config.h"
#include "munit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void restore_env(const char *name, const char *value, int had_value) {
  if (had_value)
    setenv(name, value, 1);
  else
    unsetenv(name);
}

static MunitResult test_state_dir_uses_xdg_state_home(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *old_home = getenv("HOME");
  const char *old_xdg = getenv("XDG_STATE_HOME");
  char home_copy[512];
  char xdg_copy[512];
  int had_home = old_home != NULL;
  int had_xdg = old_xdg != NULL;
  if (old_home)
    snprintf(home_copy, sizeof(home_copy), "%s", old_home);
  if (old_xdg)
    snprintf(xdg_copy, sizeof(xdg_copy), "%s", old_xdg);

  setenv("HOME", "/tmp/capstan-home", 1);
  setenv("XDG_STATE_HOME", "/tmp/capstan-state", 1);

  char path[512];
  munit_assert_int(app_state_dir(path, sizeof(path)), ==, 0);
  munit_assert_string_equal(path, "/tmp/capstan-state/capstan");

  munit_assert_int(app_state_path(path, sizeof(path), "state.lua"), ==, 0);
  munit_assert_string_equal(path, "/tmp/capstan-state/capstan/state.lua");

  restore_env("HOME", home_copy, had_home);
  restore_env("XDG_STATE_HOME", xdg_copy, had_xdg);
  return MUNIT_OK;
}

static MunitResult test_state_dir_falls_back_to_local_state(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *old_home = getenv("HOME");
  const char *old_xdg = getenv("XDG_STATE_HOME");
  char home_copy[512];
  char xdg_copy[512];
  int had_home = old_home != NULL;
  int had_xdg = old_xdg != NULL;
  if (old_home)
    snprintf(home_copy, sizeof(home_copy), "%s", old_home);
  if (old_xdg)
    snprintf(xdg_copy, sizeof(xdg_copy), "%s", old_xdg);

  setenv("HOME", "/tmp/capstan-home", 1);
  unsetenv("XDG_STATE_HOME");

  char path[512];
  munit_assert_int(app_state_dir(path, sizeof(path)), ==, 0);
  munit_assert_string_equal(path, "/tmp/capstan-home/.local/state/capstan");

  restore_env("HOME", home_copy, had_home);
  restore_env("XDG_STATE_HOME", xdg_copy, had_xdg);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/state_dir_uses_xdg_state_home", test_state_dir_uses_xdg_state_home, NULL,
     NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/state_dir_falls_back_to_local_state",
     test_state_dir_falls_back_to_local_state, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite app_config_suite = {"/app_config", tests, NULL, 1,
                               MUNIT_SUITE_OPTION_NONE};
