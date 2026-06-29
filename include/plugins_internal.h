#ifndef PLUGINS_INTERNAL_H
#define PLUGINS_INTERNAL_H

#include <lua.h>
#include <stddef.h>

int plugins_file_exists(const char *path);
char *plugins_read_file(const char *path, size_t *out_size);
int plugins_lua_dobuffer_named(lua_State *l, const char *name,
                               const char *data, size_t size);

#endif
