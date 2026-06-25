#ifndef SCROLL_H
#define SCROLL_H

int scroll_get(void);
int scroll_is_following(void);
void scroll_set(int val);
void scroll_up(int n);
void scroll_down(int n);
void scroll_reset(void);
void scroll_update_content(int total_lines, int view_height);

#endif
