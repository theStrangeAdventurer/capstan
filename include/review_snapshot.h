#ifndef REVIEW_SNAPSHOT_H
#define REVIEW_SNAPSHOT_H
#include <lua.h>
/* Registers internal adapters on the tools table at the top of the stack.
 * Enumeration is metadata-only; reads independently enforce nonprompting permit.
 * Lua owns snapshot stabilization, aggregate byte limits and attribution. */
void review_snapshot_init(lua_State *L);
#endif
