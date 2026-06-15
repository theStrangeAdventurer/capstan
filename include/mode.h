#ifndef MODE_H
#define MODE_H

#define FOCUS_INPUT    0
#define FOCUS_MESSAGES 1

int mode_get(void);
void mode_set(int focus);
void mode_toggle(void);

#endif
