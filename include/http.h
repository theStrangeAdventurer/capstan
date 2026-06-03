#ifndef HTTP_H
#define HTTP_H
#include <lua.h>
void http_init(lua_State *L);
int  http_poll(lua_State *L);
void http_cleanup(void);
int  http_is_loading(void);
#endif
