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
  char ready[128];
} StartScreenStatusLines;

StartScreenLayout start_screen_layout_for_size(int height, int width);
void start_screen_collapse_home(const char *path, char *out, size_t out_size);
void start_screen_truncate(const char *value, char *out, size_t out_size,
                           int max_chars);
void start_screen_build_status(const StartScreenStatus *status,
                               StartScreenStatusLines *out);

#define START_SCREEN_WORDMARK_ROWS 5
#define START_SCREEN_WORDMARK_LETTERS 7
#define START_SCREEN_WORDMARK_LETTER_COLUMNS 7
#define START_SCREEN_WORDMARK_LETTER_GAP 1
#define START_SCREEN_WORDMARK_COLUMNS                                           \
  (START_SCREEN_WORDMARK_LETTERS * START_SCREEN_WORDMARK_LETTER_COLUMNS +       \
   (START_SCREEN_WORDMARK_LETTERS - 1) * START_SCREEN_WORDMARK_LETTER_GAP)

int start_screen_wordmark_pixel(int row, int column);
int start_screen_wordmark_grain(int row, int column);
int start_screen_animation_tick(long long elapsed_ms);
int start_screen_gradient_level(int row, int column, int tick);

#endif
