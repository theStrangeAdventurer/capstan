#ifndef POPUP_H
#define POPUP_H

#define POPUP_DEFAULT_LIMIT 10
#define POPUP_MIN_WIDTH     20

typedef struct {
  char *text;
  char *value;
} PopupItem;

void popup_open(PopupItem *items, int count, const char *title,
                int max_visible, int multi);
int popup_is_active(void);
int popup_handle_key(int ch);
char **popup_get_selected(int *count);
void popup_free_selected(char **selected, int count);
void popup_close(void);
void popup_render(void);

#endif
