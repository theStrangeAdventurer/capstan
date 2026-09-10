#include "start_screen.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char START_SCREEN_WORDMARK[][START_SCREEN_WORDMARK_COLUMNS + 1] = {
    "011111  011110  111110  011111  111111  011110  110011",
    "111111  111111  111111  111111  111111  111111  110011",
    "110000  110011  110011  110000  001100  110011  111011",
    "110000  110011  111111  111110  001100  110011  111011",
    "110000  111111  111110  011111  001100  111111  110111",
    "110000  111111  110000  000011  001100  111111  110111",
    "111111  110011  110000  111111  001100  110011  110011",
    "011111  110011  110000  111110  001100  110011  110011",
};

int start_screen_wordmark_pixel(int row, int column) {
  if (row < 0 || row >= START_SCREEN_WORDMARK_ROWS || column < 0 ||
      column >= START_SCREEN_WORDMARK_COLUMNS)
    return 0;
  return START_SCREEN_WORDMARK[row][column] == '1';
}

int start_screen_wordmark_cell(int row, int column) {
  if (row < 0 || row >= START_SCREEN_WORDMARK_DISPLAY_ROWS)
    return 0;
  return start_screen_wordmark_pixel(row * 2, column) |
         (start_screen_wordmark_pixel(row * 2 + 1, column) << 1);
}

int start_screen_animation_tick(long long elapsed_ms) {
  const int sweep_ms = 900;
  const int pause_ms = 2700;
  const int travel = START_SCREEN_WORDMARK_COLUMNS + START_SCREEN_WORDMARK_ROWS + 24;
  long long cycle_ms = sweep_ms + pause_ms;
  long long phase = elapsed_ms % cycle_ms;
  if (phase < 0)
    phase += cycle_ms;
  if (phase >= sweep_ms)
    return travel - 1;

  long long accelerated = phase * phase * phase;
  long long duration = (long long)sweep_ms * sweep_ms * sweep_ms;
  /* Start at the visible leading edge, without a hidden off-screen run-up. */
  return 8 + (int)(accelerated * (travel - 9) / duration);
}

int start_screen_animation_frame(StartScreenAnimation *animation, int visible,
                                 long long now_ms) {
  const int resting_tick = START_SCREEN_WORDMARK_COLUMNS +
                           START_SCREEN_WORDMARK_ROWS + 23;
  if (!visible) {
    animation->visible = 0;
    return resting_tick;
  }
  if (!animation->visible) {
    animation->visible = 1;
    animation->opened_ms = now_ms;
  }
  long long elapsed_ms = now_ms - animation->opened_ms;
  if (elapsed_ms < 500)
    return resting_tick;
  return start_screen_animation_tick(elapsed_ms - 500);
}

int start_screen_gradient_level(int row, int column, int tick) {
  if (row < 0 || row >= START_SCREEN_WORDMARK_ROWS || column < 0 ||
      column >= START_SCREEN_WORDMARK_COLUMNS)
    return 0;

  int cycle = START_SCREEN_WORDMARK_COLUMNS + START_SCREEN_WORDMARK_ROWS + 24;
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

StartScreenContent start_screen_content_for_size(int height, int width) {
  StartScreenContent content = {0};
  StartScreenLayout layout = start_screen_layout_for_size(height, width);
  if (layout == START_SCREEN_MINIMAL)
    return content;

  content.width = width - 4;
  if (content.width > 56)
    content.width = 56;
  content.height = layout == START_SCREEN_WIDE
                       ? START_SCREEN_WORDMARK_DISPLAY_ROWS + 11 : 10;
  content.x = (width - content.width) / 2;
  content.y = (height - content.height) / 2;
  content.version_y = layout == START_SCREEN_WIDE
                          ? content.y + START_SCREEN_WORDMARK_DISPLAY_ROWS + 2
                          : content.y;
  content.status_y = content.y + content.height - 7;
  content.ready_y = content.y + content.height - 2;
  return content;
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

  snprintf(out->ready, sizeof(out->ready), "Type a message to begin");
  snprintf(out->shortcuts, sizeof(out->shortcuts),
           "/models choose model · Shift+Tab profiles");
}
