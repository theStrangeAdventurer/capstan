#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include "session.h"
#include <stddef.h>

int session_manager_init(const char *workspace_root);
int session_manager_init_selected(const char *workspace_root,
                                  const char *session_id);
int session_manager_new(void);
int session_manager_switch(const char *id);
int session_manager_save(void);
void session_manager_tick(void);
int session_manager_list(SessionInfo **items, size_t *count);
const char *session_manager_active_id(void);
const char *session_manager_active_title(void);
int session_manager_title_context(const char **id, const char **user_text,
                                  const char **assistant_text);
int session_manager_set_generated_title(const char *id, const char *title);
void session_manager_shutdown(void);

#endif
