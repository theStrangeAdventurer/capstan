#ifndef CAPSTAN_OAUTH_NATIVE_H
#define CAPSTAN_OAUTH_NATIVE_H
#include <lua.h>
/* Installs capstan.oauth without replacing other capstan fields.
 * poll(): raw query on first valid GET; nil while pending; nil,error on failure.
 * Invalid requests close only that connection. Listener expires after 300s;
 * incomplete connections expire after 5s (deadlines checked by poll).
 * close() is idempotent. Lua owns scheduling, cancellation and OAuth validation.
 */
void oauth_native_init(lua_State *L);
#endif
