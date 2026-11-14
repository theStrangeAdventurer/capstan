#ifndef HERDR_H
#define HERDR_H

/* Main-thread only. Call each render with the active session (or NULL) and
 * "working", "idle", or "blocked". No shell, logging, or terminal output. */
/* Optional NULL-terminated resume argv; invalid commands are omitted, not sent.
 * Caller owns argv; it is consumed synchronously before this function returns. */
void herdr_tick(const char *session_id, const char *state,
                const char *const *resume_argv);
/* Call once on normal exit, after the final tick. Bounded best-effort release. */
void herdr_shutdown(void);

#endif
