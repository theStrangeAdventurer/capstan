#ifndef LOG_H
#define LOG_H

#include <lua.h>
#include <stddef.h>

int log_path(char *buf, size_t buf_size);
int log_set_session_id(const char *session_id);
const char *log_session_id(void);
int log_event(const char *category, const char *message);
int log_event_level(const char *level, const char *category,
                    const char *message);
void log_init(lua_State *L);
void log_cleanup(void);

#endif
