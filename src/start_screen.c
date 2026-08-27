#include "start_screen.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *START_SCREEN_WORDMARK[] = {
    "0111110 0011100 1111110 0111110 1111111 0011100 1100011",
    "1100000 1100011 1100011 1100000 0011100 1100011 1111011",
    "1100000 1111111 1111110 0111110 0011100 1111111 1101111",
    "1100000 1100011 1100000 0000011 0011100 1100011 1100111",
    "0111110 1100011 1100000 1111110 0011100 1100011 1100011",
};

int start_screen_wordmark_pixel(int row, int column) {
  if (row < 0 || row >= START_SCREEN_WORDMARK_ROWS || column < 0 ||
      column >= START_SCREEN_WORDMARK_COLUMNS)
    return 0;
  return START_SCREEN_WORDMARK[row][column] == '1';
}

int start_screen_wordmark_grain(int row, int column) {
  if (!start_screen_wordmark_pixel(row, column))
    return 0;
  unsigned int hash = (unsigned int)(row + 1) * 37u +
                      (unsigned int)(column + 3) * 17u;
  return hash % 11u == 0u;
}

int start_screen_animation_tick(long long elapsed_ms) {
  const int sweep_ms = 900;
  const int pause_ms = 450;
  const int travel = START_SCREEN_WORDMARK_COLUMNS + 28;
  long long cycle_ms = sweep_ms + pause_ms;
  long long phase = elapsed_ms % cycle_ms;
  if (phase < 0)
    phase += cycle_ms;
  if (phase >= sweep_ms)
    return travel - 1;

  long long accelerated = phase * phase * phase;
  long long duration = (long long)sweep_ms * sweep_ms * sweep_ms;
  return (int)(accelerated * (travel - 1) / duration);
}

int start_screen_gradient_level(int row, int column, int tick) {
  if (row < 0 || row >= START_SCREEN_WORDMARK_ROWS || column < 0 ||
      column >= START_SCREEN_WORDMARK_COLUMNS)
    return 0;

  int cycle = START_SCREEN_WORDMARK_COLUMNS + 28;
  int highlight = tick % cycle;
  if (highlight < 0)
    highlight += cycle;
  highlight -= 14;

  int distance = column + row - highlight;
  if (distance < 0)
    distance = -distance;
  if (distance == 0)
    return 6;
  if (distance == 1)
    return 5;
  if (distance <= 3)
    return 4;
  if (distance <= 5)
    return 3;
  if (distance <= 7)
    return 2;
  return 1;
}

StartScreenLayout start_screen_layout_for_size(int height, int width) {
  if (height >= 20 && width >= 64)
    return START_SCREEN_WIDE;
  if (height >= 12 && width >= 48)
    return START_SCREEN_COMPACT;
  return START_SCREEN_MINIMAL;
}

void start_screen_collapse_home(const char *path, char *out, size_t out_size) {
  if (!out || out_size == 0)
    return;
  out[0] = '\0';
  if (!path || !path[0])
    return;

  const char *home = getenv("HOME");
  if (home && home[0]) {
    size_t home_len = strlen(home);
    if (strcmp(path, home) == 0) {
      snprintf(out, out_size, "~");
      return;
    }
    if (strncmp(path, home, home_len) == 0 && path[home_len] == '/') {
      snprintf(out, out_size, "~%s", path + home_len);
      return;
    }
  }
  snprintf(out, out_size, "%s", path);
}

void start_screen_truncate(const char *value, char *out, size_t out_size,
                           int max_chars) {
  utf8_truncate(value, out, out_size,
                max_chars > 0 ? (size_t)max_chars : 0, "...");
}

void start_screen_build_status(const StartScreenStatus *status,
                               StartScreenStatusLines *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));

  const char *provider = status ? status->provider : NULL;
  const char *model = status ? status->model : NULL;
  const char *reasoning_effort = status ? status->reasoning_effort : NULL;
  const char *profile = status ? status->profile : NULL;
  const char *workdir = status ? status->workdir : NULL;

  char model_line[sizeof(out->model)];
  if (provider && provider[0] && model && model[0])
    snprintf(model_line, sizeof(model_line), "%s/%s", provider, model);
  else if (provider && provider[0])
    snprintf(model_line, sizeof(model_line), "%s/(model unset)", provider);
  else
    snprintf(model_line, sizeof(model_line), "not configured");
  start_screen_truncate(model_line, out->model, sizeof(out->model), 32);

  start_screen_truncate(reasoning_effort && reasoning_effort[0]
                            ? reasoning_effort
                            : "default",
                        out->reasoning_effort,
                        sizeof(out->reasoning_effort), 16);

  start_screen_truncate(profile && profile[0] ? profile : "implement",
                        out->profile, sizeof(out->profile), 24);

  char collapsed[sizeof(out->workdir)];
  start_screen_collapse_home(workdir, collapsed, sizeof(collapsed));
  start_screen_truncate(collapsed[0] ? collapsed : ".", out->workdir,
                        sizeof(out->workdir), 32);

  snprintf(out->ready, sizeof(out->ready),
           "Type message · /models: choose model · Shift+Tab: profiles");
}
