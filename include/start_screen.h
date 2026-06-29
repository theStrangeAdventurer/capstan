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
  const char *profile;
  const char *workdir;
} StartScreenStatus;

typedef struct {
  char model[160];
  char profile[64];
  char workdir[160];
  char ready[64];
} StartScreenStatusLines;

StartScreenLayout start_screen_layout_for_size(int height, int width);
void start_screen_collapse_home(const char *path, char *out, size_t out_size);
void start_screen_truncate(const char *value, char *out, size_t out_size,
                           int max_chars);
void start_screen_build_status(const StartScreenStatus *status,
                               StartScreenStatusLines *out);

extern const char *START_SCREEN_ART[];
extern const int START_SCREEN_ART_LINES;

#endif
