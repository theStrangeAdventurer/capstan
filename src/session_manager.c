#include "session_manager.h"
#include "agent.h"
#include "log.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define SESSION_SAVE_DELAY_MS 500

static Session g_active = {0};
static unsigned long g_saved_revision = 0;
static long long g_dirty_since_ms = 0;
static int g_initialized = 0;
static char g_title_user_text[4096];
static char g_title_assistant_text[8192];

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void release_snapshot(void) {
  for (size_t i = 0; i < g_active.message_count; i++)
    free(g_active.messages[i].images);
  free(g_active.messages);
  g_active.messages = NULL;
  g_active.message_count = 0;
}

static int snapshot_messages(void) {
  release_snapshot();
  Messages *messages = get_messages();
  if (!messages || messages->size == 0)
    return 1;
  g_active.messages = calloc(messages->size, sizeof(SessionMessage));
  if (!g_active.messages)
    return 0;
  for (size_t i = 0; i < messages->size; i++) {
    Message *source = messages->items[i];
    if (!source || ((!source->text || !source->text[0]) &&
                    source->image_count == 0))
      continue;
    SessionMessage *target = &g_active.messages[g_active.message_count++];
    target->role = source->role == MSG_USER ? SESSION_ROLE_USER
                                            : SESSION_ROLE_ASSISTANT;
    target->text = source->text;
    target->raw_text = source->raw_text ? source->raw_text : source->text;
    if (source->image_count > 0) {
      target->images = calloc(source->image_count, sizeof(SessionImage));
      if (!target->images) {
        release_snapshot();
        return 0;
      }
      target->image_count = source->image_count;
      for (size_t image_idx = 0; image_idx < source->image_count; image_idx++) {
        target->images[image_idx].mime_type =
            source->images[image_idx].mime_type;
        target->images[image_idx].data = source->images[image_idx].data;
      }
    }
  }
  return 1;
}

static void update_title(void) {
  if (strcmp(g_active.title, "New session") != 0)
    return;
  for (size_t i = 0; i < g_active.message_count; i++) {
    if (g_active.messages[i].role == SESSION_ROLE_USER &&
        g_active.messages[i].text && g_active.messages[i].text[0]) {
      session_title_from_text(g_active.messages[i].text, g_active.title,
                              sizeof(g_active.title));
      return;
    }
  }
}

int session_manager_save(void) {
  if (!g_initialized || !g_active.id[0])
    return 0;
  if (!snapshot_messages())
    return 0;
  update_title();
  g_active.updated_at = time(NULL);
  int ok = session_save(&g_active);
  release_snapshot();
  if (ok) {
    g_saved_revision = agent_messages_revision();
    g_dirty_since_ms = 0;
  }
  return ok;
}

static void install_loaded(Session *loaded) {
  clear_messages();
  for (size_t i = 0; i < loaded->message_count; i++) {
    SessionMessage *source = &loaded->messages[i];
    char *text = source->text;
    char *raw = source->raw_text;
    source->text = NULL;
    source->raw_text = NULL;
    Messages *messages = get_messages();
    size_t previous_size = messages ? messages->size : 0;
    add_message(text, raw, source->role == SESSION_ROLE_USER ? MSG_USER
                                                             : MSG_AGENT);
    Message *installed = messages && messages->size > previous_size
                             ? messages->items[messages->size - 1]
                             : NULL;
    for (size_t image_idx = 0; installed && image_idx < source->image_count;
         image_idx++)
      message_add_image(installed, source->images[image_idx].mime_type,
                        source->images[image_idx].data);
    for (size_t image_idx = 0; image_idx < source->image_count; image_idx++) {
      free(source->images[image_idx].mime_type);
      free(source->images[image_idx].data);
    }
    free(source->images);
    source->images = NULL;
    source->image_count = 0;
  }
  session_free(&g_active);
  g_active = *loaded;
  loaded->messages = NULL;
  loaded->message_count = 0;
  release_snapshot();
  g_saved_revision = agent_messages_revision();
  g_dirty_since_ms = 0;
  log_set_session_id(g_active.id);
}

int session_manager_new(void) {
  if (!g_initialized)
    return 0;
  if (g_active.id[0] && agent_messages_revision() != g_saved_revision &&
      !session_manager_save())
    return 0;
  Session created;
  if (!session_create(&created))
    return 0;
  clear_messages();
  session_free(&g_active);
  g_active = created;
  g_saved_revision = agent_messages_revision();
  g_dirty_since_ms = 0;
  log_set_session_id(g_active.id);
  return 1;
}

int session_manager_switch(const char *id) {
  if (!g_initialized || !id || !id[0])
    return 0;
  if (strcmp(id, g_active.id) == 0)
    return 1;
  if (agent_messages_revision() != g_saved_revision && !session_manager_save())
    return 0;
  Session loaded;
  if (!session_load(id, &loaded))
    return 0;
  /* Commit the durable active pointer before mutating live memory. If this
     fails, the loaded session is discarded and the current session remains
     installed, keeping memory and disk on the same session. */
  if (!session_set_active(loaded.id)) {
    session_free(&loaded);
    return 0;
  }
  install_loaded(&loaded);
  return 1;
}

int session_manager_init(const char *workspace_root) {
  session_free(&g_active);
  g_initialized = 0;
  log_set_session_id(NULL);
  if (!session_store_init(workspace_root))
    return 0;
  g_initialized = 1;
  char active_id[SESSION_ID_SIZE];
  Session loaded;
  if (session_get_active(active_id, sizeof(active_id)) &&
      session_load(active_id, &loaded)) {
    /* Startup is installing the session already named by the durable pointer;
       it does not need to rewrite that pointer. */
    install_loaded(&loaded);
    return 1;
  }
  return session_manager_new();
}

void session_manager_tick(void) {
  if (!g_initialized || agent_messages_revision() == g_saved_revision)
    return;
  long long now = now_ms();
  if (!g_dirty_since_ms)
    g_dirty_since_ms = now;
  if (now - g_dirty_since_ms >= SESSION_SAVE_DELAY_MS)
    session_manager_save();
}

int session_manager_list(SessionInfo **items, size_t *count) {
  return g_initialized && session_list(items, count);
}

const char *session_manager_active_id(void) {
  return g_active.id;
}

const char *session_manager_active_title(void) {
  return g_active.title;
}

int session_manager_title_context(const char **id, const char **user_text,
                                  const char **assistant_text) {
  if (!g_initialized || g_active.title_generated || !id || !user_text ||
      !assistant_text)
    return 0;
  Messages *messages = get_messages();
  const char *user = NULL;
  const char *assistant = NULL;
  for (size_t i = 0; messages && i < messages->size; i++) {
    Message *message = messages->items[i];
    if (!message || !message->text || !message->text[0])
      continue;
    if (!user && message->role == MSG_USER)
      user = message->text;
    else if (user && message->role == MSG_AGENT) {
      assistant = message->text;
      break;
    }
  }
  if (!user || !assistant)
    return 0;
  utf8_truncate(user, g_title_user_text, sizeof(g_title_user_text), 1500, "…");
  utf8_truncate(assistant, g_title_assistant_text,
                sizeof(g_title_assistant_text), 3000, "…");
  *id = g_active.id;
  *user_text = g_title_user_text;
  *assistant_text = g_title_assistant_text;
  return 1;
}

int session_manager_set_generated_title(const char *id, const char *title) {
  if (!g_initialized || !id || strcmp(id, g_active.id) != 0 ||
      g_active.title_generated || !title || !title[0])
    return 0;
  char normalized[SESSION_TITLE_SIZE];
  session_title_from_text(title, normalized, sizeof(normalized));
  if (!normalized[0] || strcmp(normalized, "New session") == 0)
    return 0;
  char previous_title[SESSION_TITLE_SIZE];
  snprintf(previous_title, sizeof(previous_title), "%s", g_active.title);
  int previous_generated = g_active.title_generated;
  time_t previous_updated_at = g_active.updated_at;
  utf8_truncate(normalized, g_active.title, sizeof(g_active.title), 48, "…");
  g_active.title_generated = 1;
  if (session_manager_save())
    return 1;
  snprintf(g_active.title, sizeof(g_active.title), "%s", previous_title);
  g_active.title_generated = previous_generated;
  g_active.updated_at = previous_updated_at;
  return 0;
}

void session_manager_shutdown(void) {
  if (g_initialized && agent_messages_revision() != g_saved_revision)
    session_manager_save();
  session_free(&g_active);
  g_initialized = 0;
}
