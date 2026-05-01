#ifndef AGENT_H
#define AGENT_H

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
  int count;
  int capacity;
} Messages;

void add_message(char *text, char *raw_text, MessageRole role);
void clear_messages();

#endif
