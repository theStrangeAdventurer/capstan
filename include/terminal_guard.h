#ifndef TERMINAL_GUARD_H
#define TERMINAL_GUARD_H

/* Call before initscr(), and stop after endwin(). Best effort on non-TTYs. */
void terminal_guard_start(void);
void terminal_guard_stop(void);

#endif
