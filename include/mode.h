#ifndef MODE_H
#define MODE_H

#define FOCUS_INPUT    0
#define FOCUS_MESSAGES 1

#define APP_KEY_SHIFT_TAB 0541
#define APP_KEY_ESCAPE    27
#define APP_KEY_CTRL_F    6

int mode_get(void);
void mode_set(int focus);
void mode_toggle(void);
int mode_is_focus_toggle_key(int ch);
int mode_is_profile_cycle_key(int ch);

#endif
