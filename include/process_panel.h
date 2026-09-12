#ifndef PROCESS_PANEL_H
#define PROCESS_PANEL_H
void process_panel_open(void);
int process_panel_active(void);
int process_panel_command(const char *text);
int process_panel_key(int ch);
int process_panel_mouse(int y, int x, unsigned long buttons);
void process_panel_render(void);
void process_panel_indicator(int visible);
#endif
