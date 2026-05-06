#ifndef HTTP_H
#define HTTP_H
#include <lua.h>
void http_init(lua_State *L);     // регистрирует http.* в Lua
int  http_poll(lua_State *L);     // дёргать из main loop, возвращает 1 если был чанк
void http_cleanup(void);          // curl_multi_cleanup
#endif
