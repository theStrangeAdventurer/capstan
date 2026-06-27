#include "agent.h"
#include "munit.h"
#include <stdlib.h>
#include <string.h>

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

static MunitTest tests[] = {
    {"/append_agent_without_agent_message",
     test_append_agent_without_agent_message_creates_agent, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/activity_label", test_agent_activity_label, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite agent_suite = {"/agent", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
