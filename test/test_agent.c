#include "agent.h"
#include "log.h"
#include "munit.h"
#include "session.h"
#include "session_manager.h"
#include "utils.h"
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <lualib.h>
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

static MunitResult test_ui_only_append_stays_out_of_model_history(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  clear_messages();

  char *empty = my_strdup("");
  munit_assert_not_null(empty);
  add_message(empty, empty, MSG_AGENT);

  append_to_last_message_ui("⚙ shell\n", MSG_AGENT);
  append_to_last_message("Final answer", MSG_AGENT);

  Messages *msgs = get_messages();
  munit_assert_size(msgs->size, ==, 1);
  munit_assert_string_equal(msgs->items[0]->text,
                            "⚙ shell\nFinal answer");
  munit_assert_string_equal(msgs->items[0]->raw_text, "Final answer");
  munit_assert_ptr_not_equal(msgs->items[0]->text, msgs->items[0]->raw_text);

  clear_messages();
  return MUNIT_OK;
}

static MunitResult test_agent_activity_label(const MunitParameter params[],
                                             void *data) {
  (void)params;
  (void)data;

  agent_set_activity("Delegating");
  munit_assert_string_equal(agent_activity(), "Delegating");

  munit_assert_int64(agent_activity_elapsed_seconds(), >=, 0);

  agent_set_activity(NULL);
  munit_assert_string_equal(agent_activity(), "");
  munit_assert_int64(agent_activity_elapsed_seconds(), ==, 0);

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

static MunitResult test_agent_provider_status_includes_reasoning_effort(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = luaL_newstate();
  munit_assert_not_null(L);
  agent_init(L);
  lua_getglobal(L, "agent");
  lua_getfield(L, -1, "set_info");
  lua_pushstring(L, "openrouter");
  lua_pushstring(L, "model/id");
  lua_pushstring(L, "high");
  int rc = lua_pcall(L, 3, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_string_equal(agent_provider_name(), "openrouter");
  munit_assert_string_equal(agent_provider_model(), "model/id");
  munit_assert_string_equal(agent_reasoning_effort(), "high");

  lua_getfield(L, -1, "set_info");
  lua_pushnil(L);
  lua_pushnil(L);
  lua_pushnil(L);
  rc = lua_pcall(L, 3, 0, 0);
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_null(agent_provider_name());
  munit_assert_null(agent_provider_model());
  munit_assert_null(agent_reasoning_effort());

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
  munit_assert_true(session_manager_set_tasks("[{\"title\":\"current\"}]"));
  Session tasks_disk;
  munit_assert_true(session_load(current_id, &tasks_disk));
  munit_assert_string_equal(tasks_disk.tasks_json, session_manager_tasks());
  session_free(&tasks_disk);

  /* A directory at the temporary filename forces failure even as root. */
  char blocked[PATH_MAX];
  snprintf(blocked, sizeof(blocked), "%s/%s.jsonl.tmp.%ld",
           session_store_dir(), current_id, (long)getpid());
  munit_assert_true(session_manager_set_tasks_view(0));
  munit_assert_int(session_manager_tasks_view(), ==, 1);
  munit_assert_int(mkdir(blocked, 0700), ==, 0);
  munit_assert_false(session_manager_set_tasks_view(1));
  munit_assert_int(session_manager_tasks_view(), ==, 1);
  munit_assert_false(session_manager_set_tasks("[]"));
  munit_assert_string_equal(session_manager_tasks(), "[{\"title\":\"current\"}]");
  munit_assert_true(session_load(current_id, &tasks_disk));
  munit_assert_string_equal(tasks_disk.tasks_json, session_manager_tasks());
  session_free(&tasks_disk);
  munit_assert_int(rmdir(blocked), ==, 0);

  char *current_text = my_strdup("current message");
  munit_assert_not_null(current_text);
  add_message(current_text, current_text, MSG_USER);
  munit_assert_true(session_manager_save());
  char current_title[SESSION_TITLE_SIZE];
  snprintf(current_title, sizeof(current_title), "%s",
           session_manager_active_title());

  SessionMessage target_messages[] = {
      {SESSION_ROLE_USER, "target message", "target message", NULL, 0, {0}},
  };
  Session target = {0};
  snprintf(target.id, sizeof(target.id), "target-session");
  snprintf(target.title, sizeof(target.title), "Target");
  target.created_at = 1;
  target.updated_at = 1;
  target.messages = target_messages;
  target.message_count = 1;
  munit_assert_true(session_save(&target));

  /* CLI-owned sessions use the same immediate, transactional task adapter. */
  session_manager_tasks_session(&target);
  munit_assert_true(session_manager_set_tasks("[]"));
  Session cli_disk;
  munit_assert_true(session_load(target.id, &cli_disk));
  munit_assert_string_equal(cli_disk.tasks_json, "[]");
  session_free(&cli_disk);
  munit_assert_int(chmod(session_store_dir(), 0500), ==, 0);
  munit_assert_false(session_manager_set_tasks("[1]"));
  munit_assert_string_equal(target.tasks_json, "[]");
  munit_assert_int(chmod(session_store_dir(), 0700), ==, 0);
  session_manager_tasks_session(NULL);
  free(target.tasks_json);
  target.tasks_json = NULL;
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
  munit_assert_string_equal(session_manager_tasks(), "");
  munit_assert_int(session_manager_tasks_view(), ==, 0);
  munit_assert_true(session_manager_set_tasks_view(1));
  munit_assert_true(session_manager_set_tasks("[]"));
  munit_assert_true(session_manager_switch(current_id));
  munit_assert_int(session_manager_tasks_view(), ==, 1);
  munit_assert_string_equal(session_manager_tasks(), "[{\"title\":\"current\"}]");
  clear_messages();
  munit_assert_true(session_manager_save());
  munit_assert_string_equal(session_manager_tasks(), "[{\"title\":\"current\"}]");
  munit_assert_true(session_manager_switch(target.id));
  munit_assert_string_equal(session_manager_tasks(), "[]");
  munit_assert_true(session_manager_switch(current_id));
  munit_assert_size(get_messages()->size, ==, 0);
  munit_assert_string_equal(session_manager_tasks(), "[{\"title\":\"current\"}]");
  munit_assert_true(session_manager_set_tasks(""));
  munit_assert_true(session_load(current_id, &tasks_disk));
  munit_assert_true(!tasks_disk.tasks_json || !tasks_disk.tasks_json[0]);
  session_free(&tasks_disk);
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

static MunitResult test_selected_session_create_and_resume(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  const char *old_xdg = getenv("XDG_STATE_HOME");
  char *saved_xdg = old_xdg ? my_strdup(old_xdg) : NULL;
  char temp[] = "/tmp/capstan-selected-session-test-XXXXXX";
  int temp_fd = mkstemp(temp);
  munit_assert_int(temp_fd, >=, 0);
  close(temp_fd);
  munit_assert_int(unlink(temp), ==, 0);
  munit_assert_int(mkdir(temp, 0700), ==, 0);
  munit_assert_int(setenv("XDG_STATE_HOME", temp, 1), ==, 0);

  clear_messages();
  munit_assert_true(session_manager_init_selected(
      "/repo/selected-session", "custom key"));
  munit_assert_string_equal(session_manager_active_id(), "custom key");
  munit_assert_string_equal(session_manager_active_title(), "custom key");
  munit_assert_string_equal(log_session_id(), "custom key");
  char *text = my_strdup("persisted message");
  munit_assert_not_null(text);
  add_message(text, text, MSG_USER);
  agent_enable_shell_output(1);
  message_tag_shell_output(get_messages()->items[0], 0);
  munit_assert_size(get_messages()->items[0]->shell_output.count, ==, 1);
  char output[1024] = "⚙ shell\n\n[exit 0]\n";
  for (int i = 0; i < 25; i++) strcat(output, "строка\n");
  char *shell_text = my_strdup(output);
  add_message(shell_text, my_strdup("model context"), MSG_AGENT);
  Message *shell_message = get_messages()->items[1];
  message_tag_shell_output(shell_message, strlen("⚙ shell\n\n"));
  shell_message->shell_output.blocks[0].expanded = 1;
  munit_assert_not_null(strstr(shell_output_build(&shell_message->shell_output,
                                                 shell_message->text), "[-]"));
  lua_State *L = luaL_newstate();
  munit_assert_not_null(L);
  luaL_openlibs(L);
  agent_init(L);
  munit_assert_int(luaL_dostring(L,
      "local context = {}\n"
      "local starts, finishes = 0, 0\n"
      "package.loaded['agent.telemetry'] = {\n"
      " start = function(name, parent, attrs)\n"
      "  assert(name == 'operation' and parent == context)\n"
      "  assert(attrs.operation == 'session_save')\n"
      "  starts = starts + 1; return context end,\n"
      " finish = function(span, ok, cancelled, attrs)\n"
      "  assert(span == context and ok and not cancelled)\n"
      "  assert(attrs.duration_ms >= 0); finishes = finishes + 1 end }\n"
      "assert(agent.finish_run(context, 'other session'))\n"
      "assert(starts == 0 and finishes == 0)\n"
      "assert(agent.finish_run(context, 'custom key'))\n"
      "assert(starts == 1 and finishes == 1)\n"), ==, LUA_OK);
  munit_assert_false(agent_is_running());
  lua_close(L);
  session_manager_shutdown();
  clear_messages();

  munit_assert_true(session_manager_init_selected(
      "/repo/selected-session", "custom key"));
  munit_assert_string_equal(session_manager_active_id(), "custom key");
  Messages *messages = get_messages();
  munit_assert_size(messages->size, ==, 2);
  munit_assert_string_equal(messages->items[0]->text, "persisted message");
  munit_assert_size(messages->items[0]->shell_output.count, ==, 1);
  shell_message = messages->items[1];
  munit_assert_string_equal(shell_message->text, output);
  munit_assert_string_equal(shell_message->raw_text, "model context");
  ShellOutput *restored = &shell_message->shell_output;
  munit_assert_size(restored->count, ==, 1);
  munit_assert_false(restored->blocks[0].expanded);
  const char *view = shell_output_build(restored, shell_message->text);
  munit_assert_not_null(strstr(view, "[+] 25 lines"));
  munit_assert_null(strstr(view, "строка"));
  munit_assert_int(shell_output_control(restored, restored->blocks[0].control_start), ==, 0);
  munit_assert_true(shell_output_contains(restored, restored->blocks[0].view_start,
                                        restored->blocks[0].view_end));
  restored->blocks[0].expanded = 1;
  munit_assert_not_null(strstr(shell_output_build(restored, shell_message->text), "строка"));
  agent_enable_shell_output(0);
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

static MunitResult test_shell_output_metadata(const MunitParameter params[],
                                               void *data) {
  (void)params; (void)data;
  clear_messages();
  agent_enable_shell_output(1);
  lua_State *L = luaL_newstate();
  munit_assert_not_null(L);
  agent_init(L);
  munit_assert_int(luaL_dostring(L,
      "agent.append_ui('⚙ shell\\n  $ git status ', 'agent')\n"
      "agent.append_ui('— done\\n\\n[exit 0]\\nготово\\n', 'agent', 'shell')\n"
      "agent.append('Answer', 'agent')\n"), ==, LUA_OK);
  Message *message = get_messages()->items[0];
  munit_assert_size(message->shell_output.count, ==, 1);
  munit_assert_size(message->shell_output.blocks[0].start, ==,
                     strlen("⚙ shell\n  $ git status "));
  munit_assert_size(message->shell_output.blocks[0].end, ==,
                     strlen(message->text) - strlen("Answer"));
  munit_assert_string_equal(message->raw_text, "Answer");
  munit_assert_size(message->shell_output.count, ==, 1);
  munit_assert_size(message->shell_output.blocks[0].lines, ==, 1);
  shell_output_build(&message->shell_output, message->text);
  munit_assert_string_equal(message->raw_text, "Answer");
  /* Reallocations and additional tool results keep independent byte ranges. */
  for (int i = 0; i < 10; i++) {
    size_t start = strlen(message->text);
    munit_assert_int(luaL_dostring(L,
        "agent.append_ui('[exit 1]\\nstderr:\\nfailed\\n', 'agent', 'shell')"), ==, LUA_OK);
    munit_assert_size(message->shell_output.blocks[i + 1].start, ==, start);
  }
  munit_assert_size(message->shell_output.count, ==, 11);
  /* Plain additions/restoration never infer shell output from text. Manual
     results explicitly tag their body when the context buffer is flushed. */
  char *manual = my_strdup("Shell: pwd\n[exit 0]\n/repo");
  add_message(manual, manual, MSG_USER);
  Message *user = get_messages()->items[1];
  munit_assert_size(user->shell_output.count, ==, 0);
  message_tag_shell_output(user, strlen("Shell: pwd\n"));
  munit_assert_size(user->shell_output.count, ==, 1);
  munit_assert_ptr_equal(user->text, user->raw_text);
  agent_enable_shell_output(0);
  munit_assert_int(luaL_dostring(L,
      "agent.append_ui('headless', 'agent', 'shell')"), ==, LUA_OK);
  munit_assert_size(message->shell_output.count, ==, 11);
  lua_close(L);
  clear_messages();
  return MUNIT_OK;
}

static MunitResult test_tasks_binding(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  session_manager_shutdown();
  lua_State *L = luaL_newstate();
  munit_assert_not_null(L);
  agent_init(L);
  munit_assert_int(luaL_dostring(L, "return agent.tasks_get()"), ==, LUA_OK);
  munit_assert_string_equal(lua_tostring(L, -1), "");
  lua_settop(L, 0);
  munit_assert_int(luaL_dostring(L, "return agent.tasks_set('[]')"), ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -1));
  clear_messages();
  munit_assert_string_equal(session_manager_tasks(), "[]");
  char *large = malloc(SESSION_TASKS_MAX_BYTES + 1);
  munit_assert_not_null(large);
  memset(large, 'x', SESSION_TASKS_MAX_BYTES + 1);
  for (int i = 0; i < 4; i++) {
    lua_settop(L, 0);
    lua_getglobal(L, "agent");
    lua_getfield(L, -1, "tasks_set");
    if (i == 0) lua_pushlstring(L, "a\0b", 3);
    else if (i == 1) lua_pushlstring(L, large, SESSION_TASKS_MAX_BYTES + 1);
    else if (i == 2) lua_pushinteger(L, 42);
    else lua_pushlstring(L, large, SESSION_TASKS_MAX_BYTES);
    munit_assert_int(lua_pcall(L, 1, 1, 0), ==, LUA_OK);
    munit_assert_int(lua_toboolean(L, -1), ==, i == 3);
    if (i < 3) munit_assert_string_equal(session_manager_tasks(), "[]");
  }
  free(large);
  munit_assert_size(strlen(session_manager_tasks()), ==, SESSION_TASKS_MAX_BYTES);
  lua_settop(L, 0);
  munit_assert_int(luaL_dostring(L, "return agent.tasks_set('')"), ==, LUA_OK);
  munit_assert_true(lua_toboolean(L, -1));
  munit_assert_string_equal(session_manager_tasks(), "");
  lua_close(L);
  session_manager_shutdown();
  return MUNIT_OK;
}

static MunitResult test_tasks_policy(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  lua_State *L = luaL_newstate();
  munit_assert_not_null(L);
  luaL_openlibs(L);
  agent_init(L);
  int rc = luaL_dofile(L, "test/test_tasks.lua");
  if (rc != LUA_OK) munit_log(MUNIT_LOG_ERROR, lua_tostring(L, -1));
  munit_assert_int(rc, ==, LUA_OK);
  lua_close(L);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/tasks_policy", test_tasks_policy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/tasks_binding", test_tasks_binding, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/shell_output_metadata", test_shell_output_metadata, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/append_agent_without_agent_message",
     test_append_agent_without_agent_message_creates_agent, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/ui_only_append_stays_out_of_model_history",
     test_ui_only_append_stays_out_of_model_history, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/activity_label", test_agent_activity_label, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/profile_label", test_agent_profile_label, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/provider_status_includes_reasoning_effort",
     test_agent_provider_status_includes_reasoning_effort, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/failed_active_write_keeps_current_session",
     test_failed_active_write_keeps_current_session, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/selected_session_create_and_resume",
     test_selected_session_create_and_resume, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite agent_suite = {"/agent", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
