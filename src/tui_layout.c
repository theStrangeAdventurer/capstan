#include "tui_layout.h"
#include "tui.h"
#include "text_layout.h"
#include <stdio.h>
#include <string.h>

static int columns(const char *text) {
  return text_columns(text, strlen(text));
}

static void footer_path(const char *workdir, const char *home, int width,
                         char *out, size_t size) {
  out[0] = '\0';
  if (width < 1) return;
  char path[4096];
  const char *source = workdir ? workdir : "";
  size_t home_len = home ? strlen(home) : 0;
  int shortened = home_len > 1 && strncmp(source, home, home_len) == 0 &&
                  (source[home_len] == '/' || source[home_len] == '\0');
  size_t used = 0;
  if (shortened) { path[used++] = '~'; source += home_len; }
  /* Filenames must never emit terminal controls or partial UTF-8. */
  size_t length = strlen(source);
  for (size_t i = 0; i < length;) {
    int cells;
    size_t n = text_character(source + i, length - i, &cells);
    if (used + n >= sizeof(path)) break;
    if ((unsigned char)source[i] < 32 || source[i] == 127) path[used++] = '?';
    else { memcpy(path + used, source + i, n); used += n; }
    i += n;
  }
  path[used] = '\0';
  if (columns(path) <= width) { snprintf(out, size, "%s", path); return; }
  int prefix = width >= 6 ? (shortened && path[1] == '/' ? 2 : path[0] == '/' ? 1 : 0) : 0;
  size_t tail = (size_t)prefix;
  int remaining = columns(path + tail);
  while (tail < used && remaining > width - prefix - 1) {
    int cells;
    tail += text_character(path + tail, used - tail, &cells);
    remaining -= cells;
  }
  /* Prefer whole trailing components over a chopped parent name. */
  const char *slash = strchr(path + tail, '/');
  if (slash && slash[1]) tail = (size_t)(slash - path);
  snprintf(out, size, "%.*s…%s", prefix, path, path + tail);
}

void tui_layout_workspace_footer(int width, const char *workdir,
                                 const char *home, const WorkspaceStatus *status,
                                 TuiWorkspaceFooter *footer) {
  memset(footer, 0, sizeof(*footer));
  if (width < 7) return;
  if (status && status->state == WORKSPACE_STATUS_READY) {
    if (status->files || status->added || status->deleted) {
      snprintf(footer->files, sizeof(footer->files), "%llu %s · ",
               status->files, status->files == 1 ? "file" : "files");
      snprintf(footer->added, sizeof(footer->added), "+%llu", status->added);
      snprintf(footer->deleted, sizeof(footer->deleted), " −%llu", status->deleted);
    } else {
      snprintf(footer->files, sizeof(footer->files), "clean");
    }
  } else if (status && status->state == WORKSPACE_STATUS_ERROR) {
    snprintf(footer->files, sizeof(footer->files), "diff unavailable");
  }
  int summary = columns(footer->files) + columns(footer->added) + columns(footer->deleted);
  if (footer->added[0] && summary + 21 > width) {
    footer->files[0] = '\0';
    summary = columns(footer->added) + columns(footer->deleted);
  }
  /* In very narrow terminals keep the path instead of overlapping labels. */
  if (summary && summary + 12 > width) {
    footer->files[0] = footer->added[0] = footer->deleted[0] = '\0';
    summary = 0;
  }
  footer->summary_width = summary;
  int path_width = width - 6 - (summary ? summary + 3 : 0);
  footer_path(workdir, home, path_width, footer->path, sizeof(footer->path));
}

int tui_layout_point_in_input(int rows, int cols, int y, int x) {
  int input_y = rows - INPUT_WIN_HEIGHT - MARGIN;
  int input_w = cols - 2 * MARGIN;
  return input_y >= 0 && input_w > 0 && y >= input_y &&
         y < input_y + INPUT_WIN_HEIGHT && x >= MARGIN &&
         x < MARGIN + input_w;
}

int tui_layout_tasks_height(int available, int lines, int expanded) {
  if (!expanded || lines < 1 || available < 3) return 0;
  int height = available / 2;
  if (height < 2) height = 2;
  if (height > 9) height = 9;
  if (height > lines + 1) height = lines + 1;
  return height;
}

int tui_layout_tasks_scroll(int offset, int delta, int lines, int visible) {
  int max = lines > visible ? lines - visible : 0;
  long long next = (long long)offset + delta;
  return next < 0 ? 0 : next > max ? max : (int)next;
}
