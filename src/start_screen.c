#include "start_screen.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *START_SCREEN_ART[] = {
    "          ⣀⣠⣤⣤⠶⠶⠖⣲⣶⠶⢤⣀                ",
    "      ⣀⣤⣶⠿⠛⠉⠁  ⢰⠋⣻⣿⠉⡆⠉⢻⣆              ",
    "   ⢀⣴⣾⡿⠋⠁⣀⣤⣴⣶⡿⠿⠿⠷⠿⠿⠷⠷⣦⣠⣿⠃        ",
    "  ⣴⣿⣿⣿⣧⡶⢿⢛⣫⣥⣶⣶⣿⣛⣿⣿⣿⣿⣿⣿⣿⣧⣤⣀       ",
    "  ⠻⠿⣿⣟⣩⠷⢾⣿⡟⠛⠫⠽⢿⣦⣤⣄⣠⢀⣄⣩⣿⡿⠟⠋            ",
    "   ⢤⣼⡿⣫⣝⢻⣿⡁⡀ ⠉⠉⠉⠽⠋⠁ ⠻⡙⠻⡅             ",
    "  ⢤⣾⣿⣧⡐⠿⠘⣿⡷⠘⢗  ⣀⠴⠷⠒⢶⡶⠧⣰⡇             ",
    "   ⠐⣻⣿⣿⡗⠺⣿⡃ ⠻⣶⣮⣥⣦⡴⠶⣛⡻⢶⣤⣵⣶⠟⢀⣀⣀    ",
    " ⣤⣶⣿⣿⣿⣿⣷ ⠛⠿⣷⣦⣄⣿⡋⠁ ⠈⠉⠉ ⣿⡟⠯⣶⣿⣿⣿⠇   ",
    "  ⠉⠛⢿⣿⣿⣿⣿⣶⡄⠙⢿⣿⣿⣿⣶⣶⣶⣶⣶⣶⣿⣿⣿⣾⠭⠉⠁         ",
    "     ⠈⠛⢿⣿⣿⣿⡀ ⠙⣿⣿⡟⠋⢁⣴⣿⣿⣿⣿⣧⡄           ",
    "        ⠙⢿⣿⣧⠠⠞⠋⠁⣀⣴⣿⣿⡿⠟⠋⠉              ",
    "          ⠉⠛⠧ ⠠⠾⠟⠛⠉                   ",
};

const int START_SCREEN_ART_LINES =
    (int)(sizeof(START_SCREEN_ART) / sizeof(START_SCREEN_ART[0]));

StartScreenLayout start_screen_layout_for_size(int height, int width) {
  if (height >= 15 && width >= 81)
    return START_SCREEN_WIDE;
  if (height >= 10 && width >= 48)
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
           "type your question or / + Tab for options");
}
