#ifndef CAPSTAN_OAUTH_STORE_H
#define CAPSTAN_OAUTH_STORE_H

#include <lua.h>

/* Registers capstan.oauth.store_open without replacing other capstan fields.
 * Store paths must have existing, non-symlink parents (no '..' components).
 * Creates only the final directory, mode 0700; existing modes must match.
 * Methods return nil, fixed safe error on failure; try_lock returns false
 * when busy. read returns nil alone for missing state.json. Successful
 * write/unlock/close return true. JSON validation belongs to the caller.
 * Locks use flock on macOS/Linux, are per open handle, and are not inherited
 * as usable handles across fork. Never unlink the stable .lock file.
 * A failed write after rename may already have committed the new contents.
 */
void oauth_store_init(lua_State *L);

#endif
