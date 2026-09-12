/* Standalone C-only panel controller test; uses fake manager snapshots.
 * gcc -Iinclude -Ivendor/ncurses-install/include src/process_panel.c \
 *   test/test_process_panel.c vendor/ncurses-install/lib/libncursesw.a \
 *   vendor/ncurses-install/lib/libtinfow.a -o build/test_process_panel
 */
#include "process_panel.h"
#include "background_work.h"
#define process_manager_count background_work_count
#define process_manager_at background_work_at
#define process_manager_get background_work_get
#define process_manager_stop background_work_stop
#define process_manager_output background_work_output
#include <assert.h>
#include <ncursesw/curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int stopped;
static const char *kind = "mcp";
int background_work_inprocess(const BackgroundSnapshot *s) {
  return !strcmp(s->kind, "subagent") || !strcmp(s->kind, "subagent_group");
}
const char *background_work_status(const BackgroundSnapshot *s) { (void)s; return "running"; }
size_t process_manager_count(void) { return 1; }
int process_manager_at(size_t i, ProcessSnapshot *p) {
  if (i) return 0;
  memset(p, 0, sizeof(*p));
  strcpy(p->id, "opaque-test-id");
  strcpy(p->kind, kind);
  p->pid = 42;
  p->running = 1;
  return 1;
}
int process_manager_get(const char *id, ProcessSnapshot *p) {
  return !strcmp(id, "opaque-test-id") && process_manager_at(0, p);
}
int process_manager_stop(const char *id) {
  assert(!strcmp(id, "opaque-test-id"));
  stopped++;
  return 1;
}
char *process_manager_output(const char *id, int stream) {
  (void)id; (void)stream;
  return NULL;
}
long long process_manager_now_ms(void) { return 1000; }
int main(void) {
  assert(process_panel_command("/processes"));
  assert(process_panel_command(" \t/processes \n"));
  assert(!process_panel_command("/processes stop"));
  assert(!process_panel_command("/processes-other"));
  assert(!process_panel_command("text /processes"));
  assert(!process_panel_command("/"));
  assert(!process_panel_command(NULL));
  assert(!process_panel_key('a'));
  assert(process_panel_key(0x10));
  assert(process_panel_active());
  assert(process_panel_key('a')); /* Underlying draft never receives keys. */
  process_panel_key('s');
  assert(stopped == 0);
  process_panel_key('\n'); /* Enter is not implicit confirmation. */
  assert(stopped == 0);
  process_panel_key('n');
  process_panel_key('y');
  assert(stopped == 0);
  process_panel_key('s');
  process_panel_key('y');
  assert(stopped == 1);
  process_panel_key('\n');
  process_panel_key(27); /* Details -> list */
  assert(process_panel_active());
  process_panel_key(27);
  assert(!process_panel_active());

  FILE *out = tmpfile(), *in = tmpfile();
  assert(out && in);
  SCREEN *screen = newterm("xterm", out, in);
  assert(screen);
  const char *badge = "[ Background processes: 1 | /processes for details ]";
  const int sizes[] = {110, 80, 35, 20};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    int cols = sizes[i];
    assert(resizeterm(24, cols) == OK);
    erase();
    process_panel_indicator(1);
    assert(!process_panel_mouse(0, 2, BUTTON1_CLICKED));
    if (cols == 20) {
      assert(!process_panel_mouse(0, cols - 3, BUTTON1_CLICKED));
      continue;
    }
    const char *label = cols == 35 ? "[ Background processes: 1 ]" : badge;
    int x = cols - 1 - (int)strlen(label);
    char text[96];
    mvinnstr(0, x, text, (int)strlen(label));
    assert(!strcmp(text, label));
    assert(mvinch(0, x) & A_DIM);
    assert(!process_panel_mouse(0, x - 1, BUTTON1_CLICKED));
    assert(!process_panel_mouse(0, cols - 1, BUTTON1_CLICKED));
    assert(process_panel_mouse(0, x, BUTTON1_CLICKED));
    assert(process_panel_active());
    process_panel_key(0x10);
    process_panel_indicator(0); /* Session overlay owns this corner. */
    assert(!process_panel_mouse(0, x, BUTTON1_CLICKED));
  }
  assert(resizeterm(24, 110) == OK);
  kind = "subagent_group";
  erase(); process_panel_indicator(1);
  assert(!process_panel_mouse(0, 108, BUTTON1_CLICKED));
  kind = "subagent";
  erase(); process_panel_indicator(1);
  assert(process_panel_mouse(0, 108, BUTTON1_CLICKED));
  process_panel_key(0x10);
  endwin();
  delscreen(screen);
  fclose(out);
  fclose(in);
  return 0;
}
