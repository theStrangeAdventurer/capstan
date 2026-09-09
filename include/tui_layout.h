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
