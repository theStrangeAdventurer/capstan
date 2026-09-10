#include "tui_layout.h"
#include "tui.h"
#include "text_layout.h"
#include <stdio.h>
#include <string.h>

int tui_layout_session_height(int rows, int cols) {
  return rows >= INPUT_WIN_HEIGHT + 2 * MARGIN + 3 && cols >= 7 ? 2 : 0;
}

void tui_layout_session_row(int cols, const char *value, const char *icon,
                            TuiSessionRow *row) {
  memset(row, 0, sizeof(*row));
  if (!value || !*value || cols < 7) return;
  int budget = cols - 2 * MARGIN;
  if (budget > 60) budget = 60;
  int icon_width = text_columns(icon, strlen(icon));
  int room = budget - icon_width - 1;
  size_t len = strlen(value), used = 0, i = 0;
  int width = 0;
  while (i < len) {
    int cells;
    size_t n = text_character(value + i, len - i, &cells);
    int control = (unsigned char)value[i] < 32 || value[i] == 127;
    if (control) cells = 1;
    if (width + cells > room - 1 || used + n + 8 >= sizeof(row->text)) break;
    if (control) row->text[used++] = ' ';
    else { memcpy(row->text + used, value + i, n); used += n; }
    width += cells;
    i += n;
  }
  if (i < len) { memcpy(row->text + used, "…", 3); used += 3; }
  snprintf(row->text + used, sizeof(row->text) - used, " %s", icon);
  row->width = text_columns(row->text, strlen(row->text));
  row->x = cols - MARGIN - row->width;
}

void tui_layout_session_id_row(int cols, const char *id, const char *icon,
                               TuiSessionRow *row) {
  char labeled[512];
  snprintf(labeled, sizeof(labeled), "session.id: %s", id ? id : "");
  tui_layout_session_row(cols, id && *id ? labeled : NULL, icon, row);
}

static int columns(const char *text) {
  return text_columns(text, strlen(text));
}

/* Shared cell-safe clipping for the status row; never emit controls. */
static void status_clip(const char *text, int width, char *out, size_t size) {
  size_t len = strlen(text ? text : ""), used = 0, i = 0;
  int cells_used = 0;
  if (width < 1) { out[0] = '\0'; return; }
  /* Reserve an ellipsis only when the complete sanitized text cannot fit.
     Remaining bytes may be zero-width combining marks. */
  int total_cells = 0;
  for (size_t p = 0; p < len;) {
    int cells;
    size_t n = text_character(text + p, len - p, &cells);
    if ((unsigned char)text[p] < 32 || text[p] == 127) cells = 1;
    total_cells += cells;
    p += n;
  }
  int clipped = total_cells > width || len >= size;
  while (i < len) {
    int cells;
    size_t n = text_character(text + i, len - i, &cells);
    int control = (unsigned char)text[i] < 32 || text[i] == 127;
    if (control) cells = 1;
    if (cells_used + cells > width - clipped ||
        used + n + (clipped ? 3 : 0) >= size) break;
    if (control) out[used++] = ' ';
    else { memcpy(out + used, text + i, n); used += n; }
    cells_used += cells;
    i += n;
  }
  if (i < len && used + 3 < size) { memcpy(out + used, "…", 3); used += 3; }
  out[used] = '\0';
}

void tui_layout_status_row(int width, const char *activity, const char *profile,
                           const char *model, const char *effort, TuiStatusRow *row) {
  memset(row, 0, sizeof(*row));
  if (width < 1) return;
  status_clip(activity, width, row->activity, sizeof(row->activity));
  int left = columns(row->activity);
  int room = width - left - (left ? 3 : 0);
  if (room < 1) return;
  char p[128], m[256], e[64], candidate[512];
  status_clip(profile, 100, p, sizeof(p));
  /* The provider remains available in /info; model namespace is metadata. */
  const char *short_model = model ? strrchr(model, '/') : NULL;
  status_clip(short_model ? short_model + 1 : model, 200, m, sizeof(m));
  status_clip(effort, 40, e, sizeof(e));
  snprintf(candidate, sizeof(candidate), "%s%s%s%s%s", p, p[0] && m[0] ? " · " : "",
           m, e[0] ? " · effort " : "", e);
  if (columns(candidate) > room)
    snprintf(candidate, sizeof(candidate), "%s%s%s", p, p[0] && m[0] ? " · " : "", m);
  if (columns(candidate) > room) snprintf(candidate, sizeof(candidate), "%s", p);
  if (columns(candidate) > room) return;
  snprintf(row->metadata, sizeof(row->metadata), "%s", candidate);
  row->metadata_x = width - columns(row->metadata);
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
      snprintf(footer->files, sizeof(footer->files), "Changes: %llu %s · ",
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
