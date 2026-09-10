#ifndef TUI_LAYOUT_H
#define TUI_LAYOUT_H

#include "workspace_status.h"

typedef struct {
  char text[512];
  int x, width;
} TuiSessionRow;

int tui_layout_session_height(int rows, int cols);
void tui_layout_session_row(int cols, const char *value, const char *icon,
                            TuiSessionRow *row);

/* Display-only OTEL label; clipboard callers retain the raw ID. */
void tui_layout_session_id_row(int cols, const char *id, const char *icon,
                               TuiSessionRow *row);

typedef struct {
  char activity[512], metadata[512];
  int metadata_x, profile_width;
} TuiStatusRow;

/* Activity has priority; drop effort/model before clipping the activity. */
void tui_layout_status_row(int width, const char *activity, const char *profile,
                           const char *model, const char *effort, TuiStatusRow *row);

typedef struct {
  char path[4096];
  char files[80], added[32], deleted[32];
  int summary_width;
} TuiWorkspaceFooter;

void tui_layout_workspace_footer(int width, const char *workdir,
                                 const char *home, const WorkspaceStatus *status,
                                 TuiWorkspaceFooter *footer);
/* Bounded task viewport, including its chevron row; always leaves chat space. */
int tui_layout_tasks_height(int available, int lines, int expanded);
int tui_layout_tasks_scroll(int offset, int delta, int lines, int visible);
int tui_layout_point_in_input(int rows, int cols, int y, int x);

#endif
