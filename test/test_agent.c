#include "agent.h"
#include "log.h"
#include "munit.h"
#include "session.h"
#include "session_manager.h"
#include "utils.h"
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static MunitResult test_append_agent_without_agent_message_creates_agent(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  clear_messages();

  char *user = malloc(5);
  munit_assert_not_null(user);
  strcpy(user, "user");
  add_message(user, user, MSG_USER);

  append_to_last_message("agent", MSG_AGENT);

  Messages *msgs = get_messages();
  munit_assert_size(msgs->size, ==, 2);
  munit_assert_int(msgs->items[0]->role, ==, MSG_USER);
  munit_assert_string_equal(msgs->items[0]->text, "user");
  munit_assert_int(msgs->items[1]->role, ==, MSG_AGENT);
  munit_assert_string_equal(msgs->items[1]->text, "agent");

  clear_messages();
  return MUNIT_OK;
}

static MunitResult test_agent_activity_label(const MunitParameter params[],
                                             void *data) {
  (void)params;
  (void)data;

  agent_set_activity("Delegating");
  munit_assert_string_equal(agent_activity(), "Delegating");

  agent_set_activity(NULL);
  munit_assert_string_equal(agent_activity(), "");

  return MUNIT_OK;
}

static MunitResult test_agent_profile_label(const MunitParameter params[],
                                            void *data) {
  (void)params;
  (void)data;

  lua_State *L = luaL_newstate();
  munit_assert_not_null(L);
  agent_init(L);
  lua_getglobal(L, "agent");
  lua_getfield(L, -1, "set_profile_info");
  lua_pushstring(L, "plan");
  int rc = lua_pcall(L, 1, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(agent_profile_name(), "plan");

  lua_getfield(L, -1, "set_profile_info");
  lua_pushnil(L);
  rc = lua_pcall(L, 1, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_null(agent_profile_name());

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_failed_active_write_keeps_current_session(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *old_xdg = getenv("XDG_STATE_HOME");
  char *saved_xdg = old_xdg ? my_strdup(old_xdg) : NULL;
  char temp[] = "/tmp/capstan-session-manager-test-XXXXXX";
  int temp_fd = mkstemp(temp);
  munit_assert_int(temp_fd, >=, 0);
  close(temp_fd);
  munit_assert_int(unlink(temp), ==, 0);
  munit_assert_int(mkdir(temp, 0700), ==, 0);
  munit_assert_int(setenv("XDG_STATE_HOME", temp, 1), ==, 0);

  clear_messages();
  munit_assert_true(session_manager_init("/repo/session-manager-transaction"));
  char current_id[SESSION_ID_SIZE];
  snprintf(current_id, sizeof(current_id), "%s",
           session_manager_active_id());
  munit_assert_string_equal(log_session_id(), current_id);

  char *current_text = my_strdup("current message");
  munit_assert_not_null(current_text);
  add_message(current_text, current_text, MSG_USER);
  munit_assert_true(session_manager_save());
  char current_title[SESSION_TITLE_SIZE];
  snprintf(current_title, sizeof(current_title), "%s",
           session_manager_active_title());

  SessionMessage target_messages[] = {
      {SESSION_ROLE_USER, "target message", "target message"},
  };
  Session target = {0};
  snprintf(target.id, sizeof(target.id), "target-session");
  snprintf(target.title, sizeof(target.title), "Target");
  target.created_at = 1;
  target.updated_at = 1;
  target.messages = target_messages;
  target.message_count = 1;
  munit_assert_true(session_save(&target));

  munit_assert_int(chmod(session_store_dir(), 0500), ==, 0);
  munit_assert_false(
      session_manager_set_generated_title(current_id, "Generated title"));
  munit_assert_string_equal(session_manager_active_title(), current_title);
  munit_assert_false(session_manager_switch(target.id));
  munit_assert_string_equal(session_manager_active_id(), current_id);
  Messages *messages = get_messages();
  munit_assert_size(messages->size, ==, 1);
  munit_assert_string_equal(messages->items[0]->text, "current message");
  char disk_active[SESSION_ID_SIZE];
  munit_assert_true(session_get_active(disk_active, sizeof(disk_active)));
  munit_assert_string_equal(disk_active, current_id);
  munit_assert_string_equal(log_session_id(), current_id);

  munit_assert_int(chmod(session_store_dir(), 0700), ==, 0);
  munit_assert_true(session_manager_switch(target.id));
  munit_assert_string_equal(log_session_id(), target.id);
  session_manager_shutdown();
  clear_messages();
  if (saved_xdg) {
    munit_assert_int(setenv("XDG_STATE_HOME", saved_xdg, 1), ==, 0);
    free(saved_xdg);
  } else {
    munit_assert_int(unsetenv("XDG_STATE_HOME"), ==, 0);
  }
  char cleanup[PATH_MAX + 16];
  snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", temp);
  munit_assert_int(system(cleanup), ==, 0);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/append_agent_without_agent_message",
     test_append_agent_without_agent_message_creates_agent, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/activity_label", test_agent_activity_label, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/profile_label", test_agent_profile_label, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/failed_active_write_keeps_current_session",
     test_failed_active_write_keeps_current_session, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite agent_suite = {"/agent", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
