#ifndef LOG_H
#define LOG_H

#include <lua.h>
#include <stddef.h>

int log_path(char *buf, size_t buf_size);
int log_set_session_id(const char *session_id);
/* Borrowed current scope; copy into Span-owned storage at creation time.
 * Invalidated by log_set_session_id() or log_cleanup(). Empty means unscoped. */
const char *log_session_id(void);
/* Synchronous lifecycle adapter: same redaction, JSONL v1, locking and rotation
 * as log_event_level(). Do not call from exporter poll/shutdown.
 * session_id is the captured owner, never the current global scope; NULL/empty
 * means unscoped. Inputs are borrowed for the duration of this call.
 * IDs must be nonzero canonical lowercase hex (32 trace / 16 span characters).
 * Invalid session or correlation IDs: return 0, errno=EINVAL, write nothing.
 * Otherwise return 1 on write success, 0 on failure. */
int log_event_correlated(const char *level, const char *category,
                         const char *message, const char *session_id,
                         const char *trace_id, const char *span_id);
int log_event(const char *category, const char *message);
int log_event_level(const char *level, const char *category,
                    const char *message);
void log_init(lua_State *L);
void log_cleanup(void);

#endif
