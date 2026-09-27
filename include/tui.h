#ifndef TUI_H
#define TUI_H

#include <stddef.h>
struct MessageImage;

#define TUI_KEY_PASTE_BEGIN 0x2000
#define TUI_KEY_TASKS_COLLAPSE 0x2001
#define TUI_KEY_TASKS_EXPAND   0x2002
#define TUI_KEY_TASKS_PREV     0x2003
#define TUI_KEY_TASKS_NEXT     0x2004
#define TUI_KEY_CTRL_U     0x15
#define TUI_KEY_CTRL_V     0x16
#define TUI_KEY_CTRL_W     0x17
#define INPUT_WIN_HEIGHT   4
#define INPUT_CONTENT_LINES 2
#define MARGIN              1
#define MSG_PAD_H           1
#define MAX_BADGE_LABEL     64

typedef struct {
  char label[MAX_BADGE_LABEL];
  char *ui_result;
  char *raw_result;
  size_t shell_output_start;
  struct MessageImage *images;
  size_t image_count;
} BufferedPluginResult;

typedef struct {
  BufferedPluginResult *items;
  int size;
  int capacity;
} BufferedPluginResults;

extern BufferedPluginResults g_buffered_results;

void init_tui(void);
void render_all(void);
int tui_tasks_height(void);
int tui_session_height(void);
int tui_handle_session_mouse(int y, int x, unsigned long buttons);
int tui_handle_tasks_key(int ch);
int tui_handle_tasks_mouse(int y, int x, unsigned long buttons);
int tui_handle_shell_mouse(int y, int x, int activate);
void tui_paste_clipboard_image(void);
int tui_handle_input_shortcut(int ch);
int tui_handle_reasoning_shortcut(int ch);
int tui_focus_input_at_point(int rows, int cols, int y, int x);
void tui_pump_blocking(void);
int tui_handle_process_input(int ch);
int tui_submit_process_command(void);
int tui_handle_paste(int ch);
void buffer_plugin_result(const char *label, char *ui_result, char *raw_result,
                          size_t shell_output_start, struct MessageImage *images,
                          size_t image_count);
void buffered_results_clear(void);

const char *tui_permit_prompt(const char *tool, const char *target);
const char *tui_choice_prompt(const char *title, const char *message,
                              const char *const *choices, int count);

#endif
