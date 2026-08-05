#ifndef AGENT_H
#define AGENT_H

#include <lua.h>
#include <stddef.h>
#include "usage.h"

typedef enum {
  MSG_USER,
  MSG_AGENT,
} MessageRole;

typedef struct {
  char *mime_type;
  char *data;
} MessageImage;

typedef struct {
  MessageRole role;
  char *text;
  char *raw_text;
  MessageImage *images;
  size_t image_count;
} Message;

typedef struct {
  Message **items;
  size_t size;
  size_t capacity;
} Messages;

void add_message(char *text, char *raw_text, MessageRole role);
int message_add_image(Message *message, const char *mime_type,
                      const char *base64_data);
void append_to_last_message(const char *text, MessageRole role);
Messages *get_messages(void);
void clear_messages(void);
unsigned long agent_messages_revision(void);
void agent_init(lua_State *L);
void agent_build_and_dispatch(lua_State *L);
void agent_compact(lua_State *L);
void agent_auto_compact(lua_State *L);
int agent_should_auto_compact(lua_State *L, const char *additional_text);
const char *agent_provider_name(void);
const char *agent_provider_model(void);
const char *agent_profile_name(void);
UsageStats agent_usage(void);
void agent_reset_usage(void);

void agent_set_thinking(int active);
int  agent_is_thinking(void);
void agent_begin_run(void);
void agent_finish_run(void);
int agent_is_running(void);
void agent_set_activity(const char *label);
const char *agent_activity(void);
long long agent_activity_elapsed_seconds(void);

#endif
