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
  PopupItem *all_items;
  int all_item_count;

  int cursor;
  int scroll;
  int max_visible;
  char *title;
  int multi;
  int filterable;
  char query[256];
  int query_len;

  struct Plugin *plugin;
  size_t cmd_end;

  void *win;
  int last_rows;
  int last_cols;
} PopupState;

extern PopupState g_popup;

typedef struct {
  int active;
  int is_error;
  char *title;
  char *text;
  void *win;
  int last_rows;
  int last_cols;
  int scroll;
  long long created_at_ms;
  int auto_close_after_ms;
  int compact;
} MsgPopup;

extern MsgPopup g_msgpopup;

typedef struct {
  int visible;
  int top;
  int height;
} PopupScrollbar;

void popup_set_win_cleanup(void (*fn)(void *));
void popup_close_data(void);
long long popup_now_ms(void);
int popup_row_prefix_width(int multi);
PopupScrollbar popup_scrollbar_calc(int item_count, int visible, int scroll);

#endif
