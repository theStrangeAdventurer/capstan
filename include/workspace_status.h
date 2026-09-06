#ifndef WORKSPACE_STATUS_H
#define WORKSPACE_STATUS_H

#include <stddef.h>

typedef enum {
  WORKSPACE_STATUS_PENDING,
  WORKSPACE_STATUS_NONE,
  WORKSPACE_STATUS_READY,
  WORKSPACE_STATUS_ERROR
} WorkspaceStatusState;

typedef struct {
  WorkspaceStatusState state;
  unsigned long long files, added, deleted;
} WorkspaceStatus;

/* UI-only adapter metadata. Configure copies argv and invalidates stale workers. */
void workspace_status_configure(const char *adapter, const char *const *argv,
                                int git_format);
int workspace_status_parse_files(const char *output, WorkspaceStatus *status);
const WorkspaceStatus *workspace_status_poll(const char *workspace);
void workspace_status_shutdown(void);
/* Parse the collector's framed, quoted-path output. Fail closed on truncation. */
int workspace_status_parse(const char *output, WorkspaceStatus *status);

#endif
