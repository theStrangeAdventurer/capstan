#ifndef TUI_LAYOUT_H
#define TUI_LAYOUT_H

#include "workspace_status.h"

typedef struct {
  char path[4096];
  char files[80], added[32], deleted[32];
  int summary_width;
} TuiWorkspaceFooter;

void tui_layout_workspace_footer(int width, const char *workdir,
                                 const char *home, const WorkspaceStatus *status,
                                 TuiWorkspaceFooter *footer);
int tui_layout_point_in_input(int rows, int cols, int y, int x);

#endif
