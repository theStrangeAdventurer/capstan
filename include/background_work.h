#ifndef BACKGROUND_WORK_H
#define BACKGROUND_WORK_H
#include "process_manager.h"
/* OS records retain their native identity; in-process records have no PID. */
typedef ProcessSnapshot BackgroundSnapshot;
int background_work_register(const char *owner, const char *kind, const char *label,
                             const char *workdir, char id[PROCESS_ID_SIZE]);
/* Terminal records are immutable. Oversize sanitized output (>65536 bytes)
 * rejects the whole update, preserving the previous snapshot and output.
 * Capacity fails closed; records are retained until shutdown. Only groups
 * produce in-process completion notifications. */
int background_work_update(const char *id, const char *status, const char *output, int ok);
int background_work_cancelled(const char *id);
int background_work_inprocess(const BackgroundSnapshot *s);
const char *background_work_status(const BackgroundSnapshot *s);
size_t background_work_count(void);
int background_work_at(size_t index, BackgroundSnapshot *s);
int background_work_get(const char *id, BackgroundSnapshot *s);
int background_work_stop(const char *id);
char *background_work_output(const char *id, int stream);
int background_work_completion(const char *owner, BackgroundSnapshot *s);
/* Flag only: caller must pump scheduler at a safe Lua boundary afterwards. */
size_t background_work_cancel_owner(const char *owner);
size_t background_work_cancel_all(void);
void background_work_shutdown(void);
/* Implemented in permit.c; only ordinary runtime / explicit teardown boundaries. */
struct lua_State;
void background_work_poll_lua(struct lua_State *L);
#endif
