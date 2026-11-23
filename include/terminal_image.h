#ifndef TERMINAL_IMAGE_H
#define TERMINAL_IMAGE_H

#include <stddef.h>
#include <stdio.h>

/* Rendered width stays inside this pixel range; the pane may force it smaller. */
#define TERMINAL_IMAGE_MIN_PX 320
#define TERMINAL_IMAGE_MAX_PX 640

typedef struct {
  int cols, rows; /* Placement box in terminal cells. */
  int width_px;
} TerminalImageBox;

typedef struct {
  int row, col; /* 1-based screen cell of the image's top-left corner. */
  const char *path;
  TerminalImageBox box;
} TerminalImage;

/* Kitty graphics support derived from the environment; cached after the first
 * call. CAPSTAN_TERMINAL_IMAGES=off|on|kitty overrides detection. */
int terminal_image_enabled(void);
/* Terminal cell size in pixels; falls back to 8x16 when unreported. */
void terminal_image_cell_size(int *width, int *height);
/* 1 and the trimmed path when the whole line is an existing PNG, JPEG, GIF or
 * WebP file. Relative paths resolve against the working directory. */
int terminal_image_line(const char *line, size_t len, char *path, size_t cap);
/* Clipboard MIME type of an image file (signature-checked); 1 on success. */
int terminal_image_mime(const char *path, char *out, size_t cap);
/* Header-derived pixel dimensions; 1 on success. */
int terminal_image_size(const char *path, int *width, int *height);
/* Fits an image into a pane of avail_cols by max_rows cells, keeping the source
 * aspect ratio. 0 when the geometry is unusable. */
int terminal_image_box(int avail_cols, int max_rows, int cell_w, int cell_h,
                       int img_w, int img_h, TerminalImageBox *out);
/* Places the images; writes nothing when the set is unchanged since last call. */
void terminal_image_draw(FILE *out, const TerminalImage *items, int count);
/* Forgets cached placement and file data (session switches, tests). */
void terminal_image_reset(void);

#endif
