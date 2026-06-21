#ifndef LOG_H
#define LOG_H

#include <lua.h>
#include <stddef.h>

int log_path(char *buf, size_t buf_size);
void log_event(const char *category, const char *message);
void log_init(lua_State *L);

#endif
