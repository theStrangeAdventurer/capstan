#ifndef PROCESS_OBSERVE_H
#define PROCESS_OBSERVE_H
#include <stddef.h>
#include <sys/types.h>

typedef struct {
  pid_t pid, ppid, pgid;
  char name[128];
  int in_managed_group;
} ProcessDescendant;

/* Read-only, best-effort native ancestry observation, excluding root itself.
 * Caller obtains root from the manager and keeps it unreaped throughout.
 * Returns entries written (never more than capacity), not total descendants.
 * Zero also covers unavailable OS data, invalid input, or allocation failure.
 * No arguments/environment are read. Names have non-ASCII/control bytes replaced
 * by '?'. No signaling, waiting, lifecycle ownership, or authorization implied.
 * A false group flag means OBSERVED ONLY, not controlled. Stops belong solely
 * to the manager's root group policy; never signal these discovered PIDs.
 * Each call takes a fresh bounded snapshot: <=65536 processes, <=256 ancestry
 * edges, <=4096 results. Inaccessible, raced, too-deep and excess entries are
 * omitted. Native reads are not atomic: processes can exit/reparent immediately
 * after verification. Callers should throttle refresh (e.g. once per second).
 * Unsupported platforms return zero. No shared cache or mutable global state.
 */
size_t process_observe_descendants(pid_t root_pid, pid_t root_pgid,
                                   ProcessDescendant *out, size_t capacity);
#endif
