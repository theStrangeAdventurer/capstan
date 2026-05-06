#include "agent.h"
#include <stdlib.h>
#include <string.h>

#define MESSAGES_CAPACITY_INCREMENT 10

static Messages messages = {0};

Messages *get_messages(void) { return &messages; }

static Message *find_last_message_by_role(MessageRole role) {
  Message *m = NULL;

  if (!messages.items || !messages.count)
    return m;

  for (int i = messages.count - 1; i >= 0; i--) {
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

  if (!m)
    return;

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
  if (messages.count >= messages.capacity) {
    messages.capacity += MESSAGES_CAPACITY_INCREMENT;
    Message **tmp =
        realloc(messages.items,
                messages.capacity *
                    sizeof(Message *)); // Выделяем место под большее количество
                                        // указателей на сообщения
    if (!tmp)
      return;
    messages.items = tmp;
  }

  Message *message = malloc(sizeof(Message));

  message->text = text;
  message->raw_text = raw_text;
  message->role = role;

  messages.items[messages.count++] = message;
}

void clear_messages(void) {
  if (!messages.items || !messages.count)
    return;

  for (int i = 0; i < messages.count; i++) {
    Message *m = messages.items[i];
    if (m->text)
      free(m->text);
    if (m->raw_text && m->raw_text != m->text)
      free(m->raw_text);
    free(m);
  }

  free(messages.items);
  messages.items = NULL;
  messages.capacity = 0;
  messages.count = 0;
}
