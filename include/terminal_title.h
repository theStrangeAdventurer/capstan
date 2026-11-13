#ifndef TERMINAL_TITLE_H
#define TERMINAL_TITLE_H

#include <stddef.h>

/* Removes terminal controls, preserves UTF-8, and bounds the OSC payload. */
void terminal_title_sanitize(const char *title, char *out, size_t size);
/* Interactive TUI only; unchanged titles and non-TTY output are ignored. */
void terminal_title_update(const char *title);
void terminal_title_restore(void);

#endif
