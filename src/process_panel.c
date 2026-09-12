#include "process_panel.h"
#include "background_work.h"
#include "process_observe.h"
#include <ncursesw/curses.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int active, detail, stream, offset, confirming;
static size_t selected, first;
static int top, left, height, width, indicator_left, indicator_width;
static char stop_id[PROCESS_ID_SIZE], notice[128], selected_id[PROCESS_ID_SIZE];
static ProcessDescendant descendants[128];
static size_t descendant_count;
static char observed_id[PROCESS_ID_SIZE];
static long long observed_at;

int process_panel_command(const char *text) {
  if (!text) return 0;
  while (isspace((unsigned char)*text)) text++;
  if (strncmp(text, "/processes", 10)) return 0;
  text += 10;
  while (isspace((unsigned char)*text)) text++;
  return !*text;
}
void process_panel_open(void) {
  active = 1;
  confirming = 0;
  notice[0] = '\0';
}
int process_panel_active(void) { return active; }
static int current(ProcessSnapshot *p) {
  size_t count = background_work_count();
  if (selected_id[0]) {
    for (size_t i = 0; i < count; i++) {
      if (background_work_at(i, p) && !strcmp(p->id, selected_id)) {
        selected = i;
        return 1;
      }
    }
  }
  if (selected >= count) selected = count ? count - 1 : 0;
  if (!count || !background_work_at(selected, p)) return 0;
  snprintf(selected_id, sizeof(selected_id), "%s", p->id);
  return 1;
}
static const char *state(const ProcessSnapshot *p) {
  return background_work_status(p);
}
/* Metadata may contain controls too. Keep every field within its own row. */
static void row(WINDOW *win, int y, const char *text) {
  if (y < 1 || y >= height - 1) return;
  wmove(win, y, 2);
  for (int n = 0; *text && n < width - 4; text++, n++) {
    unsigned char c = (unsigned char)*text;
    waddch(win, c < 32 || c >= 127 ? '?' : c);
  }
}
int process_panel_key(int ch) {
  if (!active) {
    if (ch != 0x10) return 0; /* Ctrl+P */
    process_panel_open();
    return 1;
  }
  if (confirming) {
    if (ch == 'y' || ch == 'Y') {
      snprintf(notice, sizeof(notice), "%s", background_work_stop(stop_id) ?
               "Stop requested" : "Cannot stop process (it may have exited)");
      confirming = 0;
    } else if (ch == 'n' || ch == 'N' || ch == 27) confirming = 0;
    return 1;
  }
  if (ch == 27 || ch == 'q' || ch == 0x10) {
    if (detail && ch != 0x10) detail = 0;
    else active = 0;
    return 1;
  }
  ProcessSnapshot p;
  if (!current(&p)) return 1;
  int delta = ch == KEY_UP || ch == 'k' ? -1 :
              ch == KEY_DOWN || ch == 'j' ? 1 :
              ch == KEY_PPAGE ? -10 : ch == KEY_NPAGE ? 10 : 0;
  if (detail) {
    offset += delta;
    if (offset < 0) offset = 0;
  } else if (delta) {
    long long next = (long long)selected + delta;
    selected = next < 0 ? 0 : (size_t)next;
    selected_id[0] = 0;
    (void)current(&p);
  }
  if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    detail = !detail;
    offset = 0;
  }
  if (ch == '\t' && detail) { stream = (stream + 1) % 3; offset = 0; }
  if (ch == 's' && p.running && !p.stopping) {
    snprintf(stop_id, sizeof(stop_id), "%s", p.id);
    confirming = 1;
  }
  return 1;
}
void process_panel_indicator(int visible) {
  indicator_width = 0;
  if (!visible) return;
  int cols = getmaxx(stdscr);
  char text[96];
  size_t running = 0;
  ProcessSnapshot p;
  for (size_t i = 0; i < background_work_count(); i++)
    if (background_work_at(i, &p) && p.running && strcmp(p.kind, "subagent_group")) running++;
  if (!running) return;
  snprintf(text, sizeof(text), "[ Background processes: %zu | /processes for details ]", running);
  if ((int)strlen(text) > cols - 2)
    snprintf(text, sizeof(text), "[ Background processes: %zu ]", running);
  if ((int)strlen(text) > cols - 2) return;
  indicator_width = (int)strlen(text);
  indicator_left = cols - 1 - indicator_width;
  attron(A_DIM);
  mvaddnstr(0, indicator_left, text, indicator_width);
  attroff(A_DIM);
}
int process_panel_mouse(int y, int x, unsigned long buttons) {
  int click = (buttons & (BUTTON1_CLICKED | BUTTON1_RELEASED)) != 0;
  if (!active) {
    if (y != 0 || x < indicator_left || x >= indicator_left + indicator_width) return 0;
    if (click) process_panel_open();
    return 1;
  }
  if (confirming) return 1; /* Confirmation is deliberately keyboard-only. */
  if (click && y == top && x >= left + width - 5) { active = 0; return 1; }
  if (buttons & BUTTON4_PRESSED) process_panel_key(KEY_UP);
  if (buttons & BUTTON5_PRESSED) process_panel_key(KEY_DOWN);
  if (click && !detail && y >= top + 2 && y < top + height - 3 &&
      x >= left && x < left + width) {
    size_t index = first + (size_t)(y - top - 2);
    if (index < background_work_count()) {
      selected = index; selected_id[0] = 0; detail = 1; offset = 0;
    }
  }
  return 1;
}
void process_panel_render(void) {
  if (!active) return;
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  height = rows > 26 ? 26 : rows;
  width = cols > 110 ? 110 : cols;
  top = (rows - height) / 2;
  left = (cols - width) / 2;
  if (height < 8 || width < 20) return;
  WINDOW *win = newwin(height, width, top, left);
  if (!win) return;
  werase(win);
  box(win, 0, 0);
  mvwaddstr(win, 0, 2, " Processes ");
  mvwaddstr(win, 0, width - 5, "[x]");
  ProcessSnapshot p;
  int found = current(&p);
  char text[1600];
  if (confirming) {
    row(win, 2, "Stop this process group? [y/N]");
    row(win, 3, stop_id);
    ProcessSnapshot target;
    if (background_work_get(stop_id, &target) && !strcmp(target.kind, "mcp"))
      row(win, 5, "WARNING: stopping MCP disconnects its tools and may fail active requests.");
  } else if (detail && found) {
    if (background_work_inprocess(&p)) {
      stream = 0;
      snprintf(text, sizeof(text), "%s  %s  %s", p.id, p.kind, state(&p));
    } else snprintf(text, sizeof(text), "%s  PID %ld  %s  %s  exit=%d", p.id,
             (long)p.pid, p.kind, state(&p), p.exit_code);
    row(win, 1, text);
    snprintf(text, sizeof(text), "Owner: %s   Workdir: %s", p.owner, p.workdir);
    row(win, 2, text);
    row(win, 3, p.label);
    row(win, 4, background_work_inprocess(&p) ? "Output" : stream == 2 ? "Descendants (observed only; stop acts on root group)" :
        stream ? "stderr (Tab: descendants)" : "stdout (Tab: stderr / descendants)");
    if (stream == 2) {
      long long now = process_manager_now_ms();
      if (strcmp(observed_id, p.id) || now - observed_at >= 1000) {
        descendant_count = p.running ? process_observe_descendants(p.pid, p.pgid,
            descendants, sizeof(descendants) / sizeof(descendants[0])) : 0;
        snprintf(observed_id, sizeof(observed_id), "%s", p.id);
        observed_at = now;
      }
      if (!descendant_count) row(win, 5, "No verified descendants (or observation unavailable)");
      for (size_t i = (size_t)offset; i < descendant_count && i - (size_t)offset < (size_t)(height - 8); i++) {
        ProcessDescendant *child = &descendants[i];
        snprintf(text, sizeof(text), "PID %ld  PPID %ld  %s  %s",
            (long)child->pid, (long)child->ppid, child->name,
            child->in_managed_group ? "same group" : "outside group: cannot stop");
        row(win, 5 + (int)(i - (size_t)offset), text);
      }
    } else if (!p.output_available) row(win, 5, "No captured output yet (MCP stdout is protocol-owned)");
    else {
      char *output = background_work_output(p.id, stream);
      const char *line = output;
      int skip = offset, y = 5;
      while (line && *line && y < height - 3) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (skip) skip--;
        else {
          size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
          memcpy(text, line, n); text[n] = '\0'; row(win, y++, text);
        }
        line = end ? end + 1 : NULL;
      }
      free(output);
      if (p.truncated) row(win, height - 3, "[output truncated]");
    }
  } else {
    row(win, 1, "PID      COMMAND                      KIND     OWNER        ELAPSED STATE");
    int visible = height - 5;
    if (selected < first) first = selected;
    if (selected >= first + (size_t)visible) first = selected - (size_t)visible + 1;
    if (!found) row(win, 2, "No managed processes");
    for (int r = 0; r < visible; r++) {
      size_t i = first + (size_t)r;
      if (!background_work_at(i, &p)) break;
      long long elapsed = ((p.running ? process_manager_now_ms() : p.finished_ms) - p.started_ms) / 1000;
      char pid[32] = "";
      if (!background_work_inprocess(&p)) snprintf(pid, sizeof(pid), "%ld", (long)p.pid);
      snprintf(text, sizeof(text), "%-8s %-28.28s %-14.14s %-12.12s %5llds %s%s",
               pid, p.label, p.kind, p.owner, elapsed < 0 ? 0 : elapsed,
               state(&p), !p.running && p.exit_code != 0 ? " !" : "");
      if (i == selected) wattron(win, A_REVERSE);
      row(win, r + 2, text);
      wattroff(win, A_REVERSE);
    }
  }
  if (!confirming) row(win, height - 2, notice[0] ? notice :
      "Up/Down: scroll  Enter: details/back  s: stop  Esc: back/close");
  leaveok(win, TRUE);
  wnoutrefresh(win);
  delwin(win);
  curs_set(0);
}
