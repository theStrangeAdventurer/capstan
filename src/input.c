#include "input.h"
#include "utils.h"
#include <string.h>

static char g_input_buf[INPUT_BUFFER_SIZE] = {0};
static int g_cursor = 0;

void input_init(void) {
  memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
  g_cursor = 0;
}

const char *input_get_text(void) { return g_input_buf; }

int input_get_cursor(void) { return g_cursor; }

void input_insert(int ch) {
  if (g_cursor >= INPUT_BUFFER_SIZE - 1)
    return;
  g_input_buf[g_cursor++] = ch;
  g_input_buf[g_cursor] = '\0';
}

void input_set_text(const char *text) {
  strncpy(g_input_buf, text, INPUT_BUFFER_SIZE - 1);
  g_input_buf[INPUT_BUFFER_SIZE - 1] = '\0';
  g_cursor = (int)strlen(g_input_buf);
}

void input_backspace(void) {
  if (g_cursor > 0) {
    int prev_pos = get_prev_char_start(g_input_buf, g_cursor);
    int tail_len = strlen(g_input_buf + g_cursor) + 1;
    memmove(g_input_buf + prev_pos, g_input_buf + g_cursor, tail_len);
    g_cursor = prev_pos;
  }
}

void input_move_left(void) {
  if (g_cursor > 0)
    g_cursor = get_prev_char_start(g_input_buf, g_cursor);
}

void input_move_right(void) {
  if (g_input_buf[g_cursor]) {
    g_cursor++;
    while (g_input_buf[g_cursor] && (g_input_buf[g_cursor] & 0xC0) == 0x80)
      g_cursor++;
  }
}

void input_clear(void) {
  memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
  g_cursor = 0;
}
