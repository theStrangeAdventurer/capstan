#ifndef ACP_H
#define ACP_H

#include <lua.h>

int acp_run(const char *argv0, int yolo);
void acp_register(lua_State *L);

#endif
