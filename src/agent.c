#include "agent.h"
#include "dyn_arr.h"
#include "utils.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGES_CAPACITY_INCREMENT 10

static Messages messages = {0};

Messages *get_messages(void) { return &messages; }

static char *g_provider_name = NULL;
static char *g_provider_model = NULL;

static int l_agent_set_info(lua_State *L) {
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

const char *agent_provider_name(void) { return g_provider_name; }
const char *agent_provider_model(void) { return g_provider_model; }

static Message *find_last_message_by_role(MessageRole role) {
  Message *m = NULL;

  if (!messages.items || !messages.size)
    return m;

  for (int i = messages.size - 1; i >= 0; i--) {
    m = messages.items[i];
    if (m->role == role) {
      return m;
    }
  }
  return m;
}

// FIXME: пока тут код такой из предположения что text и raw_text одинаковые для
// сообщения с ролью агента, но в целом точно будут кейсы когда оно разное
void append_to_last_message(char *text, MessageRole role) {

  Message *m = find_last_message_by_role(role);

  if (!m) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    memcpy(copy, text, len + 1);
    add_message(copy, copy, role);
    return;
  }

  int old_len = strlen(m->raw_text);
  int add_len = strlen(text);

  int new_size = old_len + add_len + 1;

  char *new_ptr = realloc(m->raw_text, new_size);

  if (!new_ptr)
    return;

  m->raw_text = new_ptr;
  m->text = new_ptr;
  memcpy(m->raw_text + old_len, text, add_len);
  m->raw_text[old_len + add_len] = '\0';
}

void add_message(char *text, char *raw_text, MessageRole role) {

  Message *message = malloc(sizeof(Message));

  message->text = text;
  message->raw_text = raw_text;
  message->role = role;

  da_append(&messages, message);
}

void free_message(Message *m) {
  if (m->text)
    free(m->text);
  if (m->raw_text && m->raw_text != m->text)
    free(m->raw_text);
  free(m);
}

void clear_messages(void) { da_free_each(&messages, free_message); }

static int l_agent_append(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  MessageRole role = MSG_USER;

  if (lua_gettop(L) >= 2) {
    const char *role_str = luaL_checkstring(L, 2);
    if (strcmp(role_str, "agent") == 0)
      role = MSG_AGENT;
  }

  append_to_last_message((char *)text, role);
  return 0;
}

void agent_init(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_agent_append);
  lua_setfield(L, -2, "append");
  lua_pushcfunction(L, l_agent_set_info);
  lua_setfield(L, -2, "set_info");
  lua_setglobal(L, "agent");
}

void agent_emit(lua_State *L) {
  Messages *msgs = get_messages();
  lua_newtable(L);
  int idx = 1;
  for (size_t i = 0; i < msgs->size; i++) {
    if (!msgs->items[i]->text || msgs->items[i]->text[0] == '\0')
      continue;
    lua_newtable(L);
    lua_pushstring(L, msgs->items[i]->role == MSG_USER ? "user" : "assistant");
    lua_setfield(L, -2, "role");
    lua_pushstring(L, msgs->items[i]->text);
    lua_setfield(L, -2, "content");
    lua_rawseti(L, -2, idx++);
  }
  lua_getglobal(L, "on_messages");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
      fprintf(stderr, "on_messages: %s\n", lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  } else {
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}
