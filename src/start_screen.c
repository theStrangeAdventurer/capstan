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
  int phase = (int)(elapsed_ms % START_SCREEN_WAVE_PERIOD_MS);
  return phase < 0 ? phase + START_SCREEN_WAVE_PERIOD_MS : phase;
}

int start_screen_animation_frame(StartScreenAnimation *animation, int visible,
                                 long long now_ms) {
  if (!visible) return 0;
  if (!animation->started) {
    animation->started = 1;
    animation->opened_ms = now_ms;
  }
  long long elapsed = now_ms - animation->opened_ms;
  if (elapsed < 0) return 0;
  /* Only assembly runs at 1.5x; flicker and the original wave use real time. */
  if (elapsed < START_SCREEN_ASSEMBLY_WALL_MS) elapsed += elapsed / 2;
  else elapsed += START_SCREEN_ASSEMBLY_MS - START_SCREEN_ASSEMBLY_WALL_MS;
  if (elapsed < START_SCREEN_INTRO_MS) return (int)elapsed;
  return START_SCREEN_INTRO_MS + start_screen_animation_tick(elapsed - START_SCREEN_INTRO_MS);
}

/* Distinct, ordered fills: top, bottom, right, middle, spine, outline, reverse. */
static int assembly_order(int row, int x, int letter) {
  switch (letter) {
  case 0: return row * 6 + x;
  case 1: return (7-row) * 6 + x;
  case 2: return (5-x) * 8 + row;
  case 3: return abs(2*row-7) * 12 + row + x * 2;
  case 4: return abs(2*x-5) * 16 + x + row * 2;
  case 5: return (5-abs(2*x-5)) * 16 + x + (7-row) * 2;
  default: return (7-row) * 6 + 5-x;
  }
}

int start_screen_particle_launch(int row, int column) {
  if (!start_screen_wordmark_pixel(row, column)) return -1;
  int letter = column / 8, key = assembly_order(row, column % 8, letter);
  int rank = 0, count = 0;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 6; x++) {
      if (!start_screen_wordmark_pixel(y, letter*8+x)) continue;
      int other = assembly_order(y, x, letter);
      if (other < key || (other == key && y*6+x < row*6+column%8)) rank++;
      count++;
    }
  /* All first pixels launch at zero; all last pixels land together. */
  return rank * 2200 / (count-1);
}

int start_screen_particle_position(int row, int column, int elapsed, int *y, int *x) {
  int launch = start_screen_particle_launch(row, column);
  int age = elapsed - launch;
  *y = row + 2; *x = column + 2;
  if (launch < 0 || age < 0) return 0;
  if (age >= 120) return 2;
  int direction = (row + column % 8 + column / 8) % 4;
  int distance = direction < 2 ? 8 : 6;
  int available = direction == 0 ? *x : direction == 1 ? START_SCREEN_CANVAS_COLUMNS-1-*x :
                  direction == 2 ? *y : START_SCREEN_CANVAS_ROWS-1-*y;
  if (distance > available) distance = available;
  int offset = (distance * (120 - age) + 119) / 120;
  if (direction == 0) *x -= offset;
  if (direction == 1) *x += offset;
  if (direction == 2) *y -= offset;
  if (direction == 3) *y += offset;
  return 1;
}

static int start_screen_contact_light(int elapsed) {
  /* Weak start, brighter retry, brief voltage dip, then stable ignition. */
  static const int keys[][2] = {
    {0,1}, {110,1}, {120,4}, {170,2}, {230,1}, {360,1}, {375,7},
    {420,3}, {500,1}, {610,1}, {630,5}, {665,2}, {690,7}, {900,7}
  };
  for (size_t i = 1; i < sizeof(keys)/sizeof(keys[0]); i++) {
    if (elapsed < keys[i][0]) {
      int t = elapsed-keys[i-1][0], duration = keys[i][0]-keys[i-1][0];
      return keys[i-1][1] + (keys[i][1]-keys[i-1][1])*t/duration;
    }
  }
  return 7;
}

void start_screen_render_frame(int elapsed, StartScreenFrame *frame) {
  memset(frame, 0, sizeof(*frame));
  if (elapsed < 0) elapsed = 0;
  for (int col = 0; col < START_SCREEN_WORDMARK_COLUMNS; col++) {
    for (int row = 0; row < START_SCREEN_WORDMARK_ROWS; row++) {
      if (!start_screen_wordmark_pixel(row, col)) continue;
      int y = row + 2, x = col + 2, level = 1;
      if (elapsed < START_SCREEN_ASSEMBLY_MS) {
        int state = start_screen_particle_position(row, col, elapsed, &y, &x);
        if (!state) continue;
        level = state == 1 ? 4 : 1;
      } else if (elapsed < START_SCREEN_ASSEMBLY_MS + START_SCREEN_FLICKER_MS) {
        int t = elapsed - START_SCREEN_ASSEMBLY_MS;
        if (row == 0 && col == 1) level = start_screen_contact_light(t);
      } else if (elapsed < START_SCREEN_INTRO_MS) {
        int t = elapsed - START_SCREEN_ASSEMBLY_MS - START_SCREEN_FLICKER_MS;
        level = 7;
        if (t >= START_SCREEN_IGNITION_MS) {
          int wave = start_screen_wave_level(row, col, 0);
          level -= (7 - wave) * (t - START_SCREEN_IGNITION_MS) / START_SCREEN_SETTLE_MS;
        }
      } else {
        level = start_screen_wave_level(row, col, elapsed - START_SCREEN_INTRO_MS);
      }
      if (y >= 0 && y < START_SCREEN_CANVAS_ROWS && x >= 0 && x < START_SCREEN_CANVAS_COLUMNS &&
          level > frame->pixels[y][x]) frame->pixels[y][x] = (unsigned char)level;
    }
  }
}

int start_screen_wave_level(int row, int column, int tick) {
  if (!start_screen_wordmark_pixel(row, column)) return 0;
  /* A broad ribbon accelerates left to right, then slows off-screen.
   * Cubic Bezier (0,0), (1/3,0), (2/3,1), (1,1): linear time,
   * eased distance, with zero velocity at the seamless loop boundary.
   * No particles, dithering or isolated highlights. */
  int phase = start_screen_animation_tick(tick);
  double span = 2.0 * START_SCREEN_WORDMARK_COLUMNS;
  double t_phase = (double)phase / START_SCREEN_WAVE_PERIOD_MS;
  double travel = t_phase * t_phase * (3.0 - 2.0 * t_phase);
  /* One shade per terminal cell avoids speckled half-pixel highlights. */
  double y = (row / 2) * 2 + 0.5 - (START_SCREEN_WORDMARK_ROWS - 1) / 2.0;
  double bend = 0.9 * y + 0.10 * y * y;
  double distance = column - (span * travel + bend);
  if (distance < -span / 2) distance += span;
  if (distance > span / 2) distance -= span;
  if (distance < 0) distance = -distance;
  if (distance >= 22) return 1;
  double t = distance / 22.0;
  double light = 1 - t * t * (3 - 2 * t);
  return 1 + (int)(5 * light + 0.5);
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
                       ? START_SCREEN_CANVAS_DISPLAY_ROWS + 10 : 9;
  content.x = (width - content.width) / 2;
  content.y = (height - content.height) / 2;
  content.version_y = layout == START_SCREEN_WIDE
                          ? content.y + START_SCREEN_CANVAS_DISPLAY_ROWS + 2
                          : content.y;
  content.status_y = content.y + content.height - 6;
  content.shortcuts_y = content.y + content.height - 1;
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

  snprintf(out->shortcuts, sizeof(out->shortcuts),
           "/ commands · Shift+Tab profiles");
}
