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
struct lua_State;
/* Explicit immutable run context; never infer a parent from the active session. */
int session_manager_persistence_begin(struct lua_State *L, int context);
void session_manager_persistence_end(struct lua_State *L, int ref, int ok,
                                     long long duration_ms);
/* Borrow the CLI-owned session until detached with NULL. */
void session_manager_tasks_session(Session *session);
const char *session_manager_tasks(void);
int *session_manager_tasks_scroll(void);
int session_manager_tasks_view(void);
int session_manager_set_tasks_view(int expanded);
int session_manager_set_tasks(const char *json);
/* Session-generation token rejects late review writes after switch/detach. */
unsigned long session_manager_issues_token(void);
const char *session_manager_issues(void);
int session_manager_set_issues(const char *json, unsigned long token);
void session_manager_tick(void);
int session_manager_list(SessionInfo **items, size_t *count);
const char *session_manager_active_id(void);
const char *session_manager_active_title(void);
int session_manager_title_context(const char **id, const char **user_text,
                                  const char **assistant_text);
int session_manager_set_generated_title(const char *id, const char *title);
void session_manager_shutdown(void);

#endif
