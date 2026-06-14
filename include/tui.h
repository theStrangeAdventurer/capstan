#ifndef TUI_H
#define TUI_H

#define INPUT_WIN_HEIGHT   4
#define INPUT_CONTENT_LINES 2
#define MARGIN              1
#define MAX_BADGE_LABEL     64

typedef struct {
  char label[MAX_BADGE_LABEL];
  char *ui_result;
  char *raw_result;
} PendingContext;

typedef struct {
  PendingContext *items;
  int size;
  int capacity;
} PendingContexts;

extern PendingContexts g_pending;

void init_tui(void);
void render_all(void);
void pending_add(const char *label, char *ui_result, char *raw_result);
void pending_clear(void);

const char *tui_permit_prompt(const char *tool, const char *target);

#endif
