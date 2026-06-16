#ifndef POPUP_H
#define POPUP_H

#define POPUP_DEFAULT_LIMIT 10
#define POPUP_MIN_WIDTH     20

#include <stddef.h>

struct Plugin;

typedef struct {
  char *text;
  char *value;
} PopupItem;

void popup_open(PopupItem *items, int count, const char *title,
                int max_visible, int multi);
void popup_init(void);
void popup_open_with_plugin(PopupItem *items, int count, const char *title,
                            int max_visible, int multi, struct Plugin *plugin, size_t cmd_end);
void popup_drill_down(PopupItem *items, int count, const char *title);
int popup_is_active(void);
int popup_handle_key(int ch);
char **popup_get_selected(int *count);
void popup_free_selected(char **selected, int count);
void popup_close(void);
void popup_render(void);
struct Plugin *popup_get_plugin(void);
size_t popup_get_cmd_end(void);

void popup_show_message(const char *title, const char *text, int is_error);
int  popup_is_message_active(void);
void popup_close_message(void);
int  popup_message_handle_key(int ch);
void popup_render_message(void);

#endif
