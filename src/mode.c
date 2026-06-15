#include "mode.h"

static int g_focus = FOCUS_INPUT;

int mode_get(void) { return g_focus; }

void mode_set(int focus) { g_focus = focus; }

void mode_toggle(void) {
    g_focus = (g_focus == FOCUS_INPUT) ? FOCUS_MESSAGES : FOCUS_INPUT;
}
