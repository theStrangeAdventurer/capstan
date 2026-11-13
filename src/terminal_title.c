#include "terminal_title.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

static char last_title[256];
static int title_saved;

void terminal_title_sanitize(const char *title, char *out, size_t size) {
  if (!out || !size) return;
  size_t used = 0;
  while (title && *title) {
    const unsigned char *bytes = (const unsigned char *)title;
    unsigned char first = bytes[0];
    size_t n = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
               first >= 0xe0 && first <= 0xef ? 3 :
               first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    uint32_t ch = first & (n == 1 ? 0x7f : n == 2 ? 0x1f : n == 3 ? 0x0f : 7);
    size_t i = 1;
    for (; i < n; i++) {
      if ((bytes[i] & 0xc0) != 0x80) break;
      ch = (ch << 6) | (bytes[i] & 0x3f);
    }
    if (!n || i != n || (n == 3 && ch < 0x800) ||
        (n == 4 && ch < 0x10000) || (ch >= 0xd800 && ch <= 0xdfff) ||
        ch > 0x10ffff) {
      title++;
      continue;
    }
    /* Decode UTF-8 independently of LC_CTYPE; C0/C1 include OSC controls. */
    if (ch >= 0x20 && !(ch >= 0x7f && ch <= 0x9f)) {
      if (n >= size - used) break;
      memcpy(out + used, title, n);
      used += n;
    }
    title += n;
  }
  out[used] = '\0';
}

void terminal_title_update(const char *title) {
  if (!isatty(STDOUT_FILENO)) return;
  char safe[sizeof(last_title)];
  terminal_title_sanitize(title && title[0] ? title : "Capstan", safe, sizeof(safe));
  if (title_saved && strcmp(last_title, safe) == 0) return;
  if (!title_saved) {
    /* Xterm title stack preserves the shell's title where supported. */
    if (atexit(terminal_title_restore) != 0) return;
    fputs("\033[22;0t", stdout);
    title_saved = 1;
  }
  fprintf(stdout, "\033]0;%s\007", safe);
  fflush(stdout);
  memcpy(last_title, safe, strlen(safe) + 1);
}

void terminal_title_restore(void) {
  if (!title_saved) return;
  /* Clear stale names in terminals (including Herdr) without a title stack. */
  fputs("\033]0;\007\033[23;0t", stdout);
  fflush(stdout);
  title_saved = 0;
  last_title[0] = '\0';
}
