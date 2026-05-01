#include "agent.h"
#include <stdlib.h>

#define MESSAGES_CAPACITY_INCREMENT 10

static Messages messages = {0};

Messages *get_messages(void) { return &messages; }

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
    if (m->raw_text)
      free(m->raw_text);
    free(m);
  }

  free(messages.items);
  messages.items = NULL;
  messages.capacity = 0;
  messages.count = 0;
}
