#ifndef START_SCREEN_H
#define START_SCREEN_H

#include <stddef.h>

typedef enum {
  START_SCREEN_MINIMAL,
  START_SCREEN_COMPACT,
  START_SCREEN_WIDE,
} StartScreenLayout;

typedef struct {
  const char *provider;
  const char *model;
  const char *reasoning_effort;
  const char *profile;
  const char *workdir;
} StartScreenStatus;

typedef struct {
  char model[160];
  char reasoning_effort[32];
  char profile[64];
  char workdir[160];
  char shortcuts[128];
} StartScreenStatusLines;

typedef struct {
  int x, y, width, height;
  int version_y, status_y, shortcuts_y;
} StartScreenContent;

StartScreenLayout start_screen_layout_for_size(int height, int width);
StartScreenContent start_screen_content_for_size(int height, int width);
void start_screen_collapse_home(const char *path, char *out, size_t out_size);
void start_screen_truncate(const char *value, char *out, size_t out_size,
                           int max_chars);
void start_screen_build_status(const StartScreenStatus *status,
                               StartScreenStatusLines *out);

#define START_SCREEN_WORDMARK_ROWS 8
#define START_SCREEN_WAVE_PERIOD_MS 2500
#define START_SCREEN_WORDMARK_DISPLAY_ROWS (START_SCREEN_WORDMARK_ROWS / 2)
#define START_SCREEN_WORDMARK_LETTERS 7
#define START_SCREEN_WORDMARK_LETTER_COLUMNS 6
#define START_SCREEN_WORDMARK_LETTER_GAP 2
#define START_SCREEN_WORDMARK_COLUMNS                                           \
  (START_SCREEN_WORDMARK_LETTERS * START_SCREEN_WORDMARK_LETTER_COLUMNS +       \
   (START_SCREEN_WORDMARK_LETTERS - 1) * START_SCREEN_WORDMARK_LETTER_GAP)

int start_screen_wordmark_pixel(int row, int column);
/* Terminal cell mask: bit 0 = upper half, bit 1 = lower half. */
int start_screen_wordmark_cell(int row, int column);
int start_screen_animation_tick(long long elapsed_ms);
/* Zero-initialize; opening is played once per application lifetime. */
typedef struct {
  int started;
  long long opened_ms;
} StartScreenAnimation;
int start_screen_animation_frame(StartScreenAnimation *animation, int visible,
                                 long long now_ms);
/* Masked silk wave: 0 = outside letters, 1..6 = increasing brightness. */
int start_screen_wave_level(int row, int column, int tick);

#define START_SCREEN_ASSEMBLY_MS 2600
#define START_SCREEN_ASSEMBLY_WALL_MS ((START_SCREEN_ASSEMBLY_MS * 2 + 2) / 3)
#define START_SCREEN_FLICKER_MS 900
#define START_SCREEN_IGNITION_MS 200
#define START_SCREEN_SETTLE_MS 300
#define START_SCREEN_INTRO_MS (START_SCREEN_ASSEMBLY_MS + START_SCREEN_FLICKER_MS + START_SCREEN_IGNITION_MS + START_SCREEN_SETTLE_MS)
#define START_SCREEN_CANVAS_ROWS 12
#define START_SCREEN_CANVAS_COLUMNS (START_SCREEN_WORDMARK_COLUMNS + 4)
#define START_SCREEN_CANVAS_DISPLAY_ROWS (START_SCREEN_CANVAS_ROWS / 2)
typedef struct {
  unsigned char pixels[START_SCREEN_CANVAS_ROWS][START_SCREEN_CANVAS_COLUMNS];
} StartScreenFrame;
/* Logical launch time in milliseconds, or -1 outside the bitmap. */
int start_screen_particle_launch(int row, int column);
/* Returns 0 before launch, 1 in flight, 2 at destination. Coordinates include padding. */
int start_screen_particle_position(int row, int column, int elapsed,
                                  int *y, int *x);
void start_screen_render_frame(int elapsed, StartScreenFrame *frame);

#endif
