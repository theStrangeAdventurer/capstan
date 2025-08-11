#ifndef TUI_H
#define TUI_H

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
} BufferedPluginResult;

typedef struct {
  BufferedPluginResult *items;
  int size;
  int capacity;
} BufferedPluginResults;

extern BufferedPluginResults g_buffered_results;

void init_tui(void);
void render_all(void);
void tui_paste_clipboard_image(void);
int tui_handle_input_shortcut(int ch);
int tui_focus_input_at_point(int rows, int cols, int y, int x);
void tui_pump_blocking(void);
void buffer_plugin_result(const char *label, char *ui_result, char *raw_result);
void buffered_results_clear(void);

const char *tui_permit_prompt(const char *tool, const char *target);

#endif
