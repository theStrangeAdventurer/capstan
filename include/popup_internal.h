#ifndef POPUP_INTERNAL_H
#define POPUP_INTERNAL_H

#include "popup.h"
#include <stddef.h>

#define POPUP_KEY_UP    0403
#define POPUP_KEY_DOWN  0402
#define POPUP_KEY_LEFT  0404
#define POPUP_KEY_RIGHT 0405

typedef struct {
  int active;
  int cancelled;

  PopupItem *items;
  int item_count;
  int *selected;

  int cursor;
  int scroll;
  int max_visible;
  char *title;
  int multi;

  struct Plugin *plugin;
  size_t cmd_end;

  void *win;
  int last_rows;
  int last_cols;
} PopupState;

extern PopupState g_popup;

void popup_set_win_cleanup(void (*fn)(void *));
void popup_close_data(void);

#endif
