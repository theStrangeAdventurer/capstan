#include "app_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

const char *APP_NAME = "capstan";
const char *APP_BINARY_NAME = "capstan";
const char *APP_CONFIG_DIR_NAME = "capstan";
const char *APP_EDITOR_TEMP_TEMPLATE = "/tmp/capstan-editor-XXXXXX";
const char *APP_BANNER_TITLE = "◉ CAPSTAN";
const char *APP_BANNER_TAGLINE = "pull context. hold course. ship code.";

int app_config_dir(char *buf, size_t buf_size) {
  const char *home = getenv("HOME");
  if (!home || !home[0] || buf_size == 0)
    return -1;

  int n = snprintf(buf, buf_size, "%s/.config/%s", home, APP_CONFIG_DIR_NAME);
  if (n < 0 || (size_t)n >= buf_size)
    return -1;
  return 0;
}

int app_config_path(char *buf, size_t buf_size, const char *relative_path) {
  char dir[512];
  if (app_config_dir(dir, sizeof(dir)) != 0)
    return -1;

  int n = snprintf(buf, buf_size, "%s/%s", dir, relative_path);
  if (n < 0 || (size_t)n >= buf_size)
    return -1;
  return 0;
}

int app_config_ensure_dir(void) {
  const char *home = getenv("HOME");
  if (!home || !home[0])
    return -1;

  char base[512];
  int n = snprintf(base, sizeof(base), "%s/.config", home);
  if (n < 0 || (size_t)n >= sizeof(base))
    return -1;
  mkdir(base, 0755);

  char dir[512];
  if (app_config_dir(dir, sizeof(dir)) != 0)
    return -1;
  mkdir(dir, 0755);
  return 0;
}
