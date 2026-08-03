#include "agent.h"
#include "dyn_arr.h"
#include "popup.h"
#include "session_manager.h"
#include "utils.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Messages messages = {0};
static unsigned long g_messages_revision = 0;

Messages *get_messages(void) { return &messages; }
unsigned long agent_messages_revision(void) { return g_messages_revision; }

static char *g_provider_name = NULL;
static char *g_provider_model = NULL;
static char *g_profile_name = NULL;
static char *g_activity = NULL;
static long long g_activity_started_ms = 0;
static int g_thinking = 0;
static int g_running = 0;
static UsageStats g_usage = {0};

void agent_set_thinking(int active) { g_thinking = active; }
int  agent_is_thinking(void)      { return g_thinking; }
void agent_begin_run(void) { g_running = 1; }
void agent_finish_run(void) { g_running = 0; }
int agent_is_running(void) { return g_running; }

static int l_agent_finish_run(lua_State *L) {
  (void)L;
  agent_finish_run();
  return 0;
}

static int l_agent_session_title_context(lua_State *L) {
  const char *id = NULL;
  const char *user_text = NULL;
  const char *assistant_text = NULL;
  if (!session_manager_title_context(&id, &user_text, &assistant_text))
    return 0;
  lua_pushstring(L, id);
  lua_pushstring(L, user_text);
  lua_pushstring(L, assistant_text);
  return 3;
}

static int l_agent_set_session_title(lua_State *L) {
  const char *id = luaL_checkstring(L, 1);
  const char *title = luaL_checkstring(L, 2);
  lua_pushboolean(L, session_manager_set_generated_title(id, title));
  return 1;
}

static int l_agent_set_thinking(lua_State *L) {
  agent_set_thinking(lua_toboolean(L, 1));
  return 0;
}

static long long monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

void agent_set_activity(const char *label) {
  free(g_activity);
  g_activity = NULL;
  g_activity_started_ms = 0;
  if (label && label[0]) {
    g_activity = my_strdup(label);
    if (g_activity)
      g_activity_started_ms = monotonic_ms();
  }
}

const char *agent_activity(void) { return g_activity ? g_activity : ""; }

long long agent_activity_elapsed_seconds(void) {
  if (!g_activity || g_activity_started_ms <= 0)
    return 0;
  long long elapsed_ms = monotonic_ms() - g_activity_started_ms;
  return elapsed_ms > 0 ? elapsed_ms / 1000LL : 0;
}

static int l_agent_set_activity(lua_State *L) {
  if (lua_isnoneornil(L, 1))
    agent_set_activity(NULL);
  else
    agent_set_activity(luaL_checkstring(L, 1));
  return 0;
}

static int l_agent_set_provider_info(lua_State *L) {
  free(g_provider_name);
  free(g_provider_model);
  g_provider_name = NULL;
  g_provider_model = NULL;
  if (!lua_isnoneornil(L, 1))
    g_provider_name = my_strdup(luaL_checkstring(L, 1));
  if (!lua_isnoneornil(L, 2))
    g_provider_model = my_strdup(luaL_checkstring(L, 2));
  return 0;
}

static int l_agent_set_profile_info(lua_State *L) {
  free(g_profile_name);
  g_profile_name = NULL;
  if (!lua_isnoneornil(L, 1))
    g_profile_name = my_strdup(luaL_checkstring(L, 1));
  return 0;
}

const char *agent_provider_name(void) { return g_provider_name; }
const char *agent_provider_model(void) { return g_provider_model; }
const char *agent_profile_name(void) { return g_profile_name; }
UsageStats agent_usage(void) { return g_usage; }
void agent_reset_usage(void) { g_usage = (UsageStats){0, 0, 0, 0}; }

static int l_agent_set_usage(lua_State *L) {
  g_usage.prompt_tokens = (int)luaL_optinteger(L, 1, 0);
  g_usage.completion_tokens = (int)luaL_optinteger(L, 2, 0);
  g_usage.total_tokens = (int)luaL_optinteger(L, 3, 0);
  g_usage.context_limit = (int)luaL_optinteger(L, 4, 0);
  return 0;
}

static Message *find_last_message_by_role(MessageRole role) {
  if (!messages.items || !messages.size)
    return NULL;

  for (int i = messages.size - 1; i >= 0; i--) {
    Message *m = messages.items[i];
    if (m->role == role) {
      return m;
    }
  }
  return NULL;
}

void append_to_last_message(const char *text, MessageRole role) {
  if (!text)
    return;

  Message *m = find_last_message_by_role(role);

  if (!m) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy)
      return;
    memcpy(copy, text, len + 1);
    add_message(copy, copy, role);
    return;
  }

  size_t old_len = m->raw_text ? strlen(m->raw_text) : 0;
  size_t add_len = strlen(text);

  if (add_len > ((size_t)-1) - old_len - 1)
    return;

  char *new_ptr = realloc(m->raw_text, old_len + add_len + 1);

  if (!new_ptr)
    return;

  m->raw_text = new_ptr;
  m->text = new_ptr;
  memcpy(m->raw_text + old_len, text, add_len);
  m->raw_text[old_len + add_len] = '\0';
  g_messages_revision++;
}

void add_message(char *text, char *raw_text, MessageRole role) {

  Message *message = malloc(sizeof(Message));
  if (!message) {
    free(text);
    if (raw_text && raw_text != text)
      free(raw_text);
    return;
  }

  message->text = text;
  message->raw_text = raw_text;
  message->role = role;

  da_append(&messages, message);
  g_messages_revision++;
}

void free_message(Message *m) {
  if (m->text)
    free(m->text);
  if (m->raw_text && m->raw_text != m->text)
    free(m->raw_text);
  free(m);
}

void clear_messages(void) {
  da_free_each(&messages, free_message);
  g_messages_revision++;
}

static int l_agent_replace_compacted_context(lua_State *L) {
  const char *summary = luaL_checkstring(L, 1);
  size_t raw_len = snprintf(NULL, 0,
                            "Previous conversation was compacted. Continue "
                            "from this state:\n\n%s",
                            summary);
  char *raw = malloc(raw_len + 1);
  if (!raw)
    return 0;
  snprintf(raw, raw_len + 1,
           "Previous conversation was compacted. Continue from this state:\n\n%s",
           summary);

  size_t ui_len = snprintf(NULL, 0, "[context compacted]\n\n%s", summary);
  char *ui = malloc(ui_len + 1);
  if (!ui) {
    free(raw);
    return 0;
  }
  snprintf(ui, ui_len + 1, "[context compacted]\n\n%s", summary);

  clear_messages();
  add_message(ui, raw, MSG_USER);
  return 0;
}

static int l_agent_append(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  MessageRole role = MSG_USER;

  if (lua_gettop(L) >= 2) {
    const char *role_str = luaL_checkstring(L, 2);
    if (strcmp(role_str, "agent") == 0)
      role = MSG_AGENT;
  }

  append_to_last_message(text, role);
  return 0;
}

void agent_init(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_agent_append);
  lua_setfield(L, -2, "append");
  lua_pushcfunction(L, l_agent_set_provider_info);
  lua_setfield(L, -2, "set_info");
  lua_pushcfunction(L, l_agent_set_profile_info);
  lua_setfield(L, -2, "set_profile_info");
  lua_pushcfunction(L, l_agent_set_thinking);
  lua_setfield(L, -2, "set_thinking");
  lua_pushcfunction(L, l_agent_finish_run);
  lua_setfield(L, -2, "finish_run");
  lua_pushcfunction(L, l_agent_session_title_context);
  lua_setfield(L, -2, "session_title_context");
  lua_pushcfunction(L, l_agent_set_session_title);
  lua_setfield(L, -2, "set_session_title");
  lua_pushcfunction(L, l_agent_set_activity);
  lua_setfield(L, -2, "set_activity");
  lua_pushcfunction(L, l_agent_set_usage);
  lua_setfield(L, -2, "set_usage");
  lua_pushcfunction(L, l_agent_replace_compacted_context);
  lua_setfield(L, -2, "replace_compacted_context");
  lua_setglobal(L, "agent");
}

static void push_messages_table(lua_State *L) {
  Messages *msgs = get_messages();
  lua_newtable(L);
  int idx = 1;
  for (size_t i = 0; i < msgs->size; i++) {
    if (!msgs->items[i]->text || msgs->items[i]->text[0] == '\0')
      continue;
    lua_newtable(L);
    lua_pushstring(L, msgs->items[i]->role == MSG_USER ? "user" : "assistant");
    lua_setfield(L, -2, "role");
    lua_pushstring(L, msgs->items[i]->raw_text);
    lua_setfield(L, -2, "content");
    lua_rawseti(L, -2, idx++);
  }
}

void agent_build_and_dispatch(lua_State *L) {
  agent_begin_run();
  push_messages_table(L);
  lua_getglobal(L, "agent_entry");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
      agent_finish_run();
      popup_show_message("Agent Error", lua_tostring(L, -1), 1);
      lua_pop(L, 1);
    }
  } else {
    agent_finish_run();
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

int agent_should_auto_compact(lua_State *L, const char *additional_text) {
  lua_getglobal(L, "should_auto_compact");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  push_messages_table(L);
  lua_pushstring(L, additional_text ? additional_text : "");
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
    lua_pop(L, 1);
    return 0;
  }
  int should_compact = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return should_compact;
}

static void agent_compact_internal(lua_State *L, int automatic) {
  Messages *msgs = get_messages();
  int non_empty = 0;
  for (size_t i = 0; i < msgs->size; i++) {
    if (msgs->items[i]->text && msgs->items[i]->text[0] != '\0')
      non_empty++;
  }
  if (non_empty == 0) {
    popup_show_message("Compact", "No conversation to compact", 0);
    return;
  }

  /* Compact is a top-level operation: keep normal submissions in the existing
     FIFO until the compacted context has replaced the old history. */
  agent_begin_run();
  push_messages_table(L);
  lua_getglobal(L, "compact_entry");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -2);
    lua_pushboolean(L, automatic);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      agent_finish_run();
      popup_show_message("Compact Error", lua_tostring(L, -1), 1);
      lua_pop(L, 1);
    }
  } else {
    agent_finish_run();
    popup_show_message("Compact Error", "Compact runtime is not initialized", 1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

void agent_compact(lua_State *L) { agent_compact_internal(L, 0); }

void agent_auto_compact(lua_State *L) { agent_compact_internal(L, 1); }
