#include "start_screen.h"
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

static int utf8_chars(const char *s) {
  int chars = 0;
  if (!s)
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if ((*p & 0xC0) != 0x80)
      chars++;
  }
  return chars;
}

static size_t bytes_for_chars(const char *s, int max_chars) {
  size_t bytes = 0;
  int chars = 0;
  if (!s || max_chars <= 0)
    return 0;
  while (s[bytes]) {
    if (((unsigned char)s[bytes] & 0xC0) != 0x80) {
      if (chars >= max_chars)
        break;
      chars++;
    }
    bytes++;
  }
  return bytes;
}

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
  if (!out || out_size == 0)
    return;
  out[0] = '\0';
  if (!value || !value[0] || max_chars <= 0)
    return;

  if (utf8_chars(value) <= max_chars) {
    snprintf(out, out_size, "%s", value);
    return;
  }

  if (max_chars <= 3) {
    size_t n = bytes_for_chars(value, max_chars);
    if (n >= out_size)
      n = out_size - 1;
    memcpy(out, value, n);
    out[n] = '\0';
    return;
  }

  size_t n = bytes_for_chars(value, max_chars - 3);
  if (n + 4 > out_size)
    n = out_size > 4 ? out_size - 4 : 0;
  memcpy(out, value, n);
  out[n] = '\0';
  strncat(out, "...", out_size - strlen(out) - 1);
}

void start_screen_build_status(const StartScreenStatus *status,
                               StartScreenStatusLines *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));

  const char *provider = status ? status->provider : NULL;
  const char *model = status ? status->model : NULL;
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

  start_screen_truncate(profile && profile[0] ? profile : "implement",
                        out->profile, sizeof(out->profile), 24);

  char collapsed[sizeof(out->workdir)];
  start_screen_collapse_home(workdir, collapsed, sizeof(collapsed));
  start_screen_truncate(collapsed[0] ? collapsed : ".", out->workdir,
                        sizeof(out->workdir), 32);

  snprintf(out->ready, sizeof(out->ready),
           "type your question or / + Tab for options");
}
