#ifndef AGENT_H
#define AGENT_H

#include <lua.h>
#include <stddef.h>

typedef enum {
  MSG_USER,
  MSG_AGENT,
} MessageRole;

typedef struct {
  MessageRole role;
  char *text;
  char *raw_text;
} Message;

typedef struct {
  Message **items;
  size_t size;
  size_t capacity;
} Messages;

void add_message(char *text, char *raw_text, MessageRole role);
void append_to_last_message(char *text, MessageRole role);
Messages *get_messages(void);
void clear_messages(void);
void agent_init(lua_State *L);
void agent_emit(lua_State *L);
const char *agent_provider_name(void);
const char *agent_provider_model(void);

void agent_set_thinking(int active);
int  agent_is_thinking(void);

#endif
