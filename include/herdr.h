#ifndef HERDR_H
#define HERDR_H

/* Main-thread only. Call each render with the active session (or NULL) and
 * "working", "idle", or "blocked". No shell, logging, or terminal output. */
void herdr_tick(const char *session_id, const char *state);
/* Call once on normal exit, after the final tick. Bounded best-effort release. */
void herdr_shutdown(void);

#endif
