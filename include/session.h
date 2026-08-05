#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>
#include <time.h>

#define SESSION_ID_SIZE 64
#define SESSION_TITLE_SIZE 96

typedef enum {
  SESSION_ROLE_USER,
  SESSION_ROLE_ASSISTANT,
} SessionRole;

typedef struct {
  char *mime_type;
  char *data;
} SessionImage;

typedef struct {
  SessionRole role;
  char *text;
  char *raw_text;
  SessionImage *images;
  size_t image_count;
} SessionMessage;

typedef struct {
  char id[SESSION_ID_SIZE];
  char title[SESSION_TITLE_SIZE];
  int title_generated;
  time_t created_at;
  time_t updated_at;
  SessionMessage *messages;
  size_t message_count;
} Session;

typedef struct {
  char id[SESSION_ID_SIZE];
  char title[SESSION_TITLE_SIZE];
  int title_generated;
  time_t created_at;
  time_t updated_at;
} SessionInfo;

int session_store_init(const char *workspace_root);
const char *session_store_dir(void);
int session_id_valid(const char *id);
int session_create(Session *session);
int session_create_named(Session *session, const char *id);
int session_save(const Session *session);
int session_load(const char *id, Session *session);
int session_delete(const char *id);
int session_list(SessionInfo **items, size_t *count);
int session_set_active(const char *id);
int session_get_active(char *id, size_t id_size);
void session_free(Session *session);
void session_list_free(SessionInfo *items);
void session_title_from_text(const char *text, char *title, size_t title_size);
unsigned long session_workspace_hash(const char *text);

#endif
