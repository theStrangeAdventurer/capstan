#include "app_config.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

const char *APP_NAME = "capstan";
const char *APP_BINARY_NAME = "capstan";
const char *APP_CONFIG_DIR_NAME = "capstan";
const char *APP_EDITOR_TEMP_TEMPLATE = "/tmp/capstan-editor-XXXXXX";
#ifndef APP_VERSION_VALUE
#define APP_VERSION_VALUE "local"
#endif
const char *APP_VERSION = APP_VERSION_VALUE;
const char *APP_BANNER_TITLE = "◉ CAPSTAN";
const char *APP_BANNER_TAGLINE = "pull context. hold course. ship code.";

static char g_workdir[PATH_MAX] = "";

static int is_absolute_path(const char *path) {
  return path && path[0] == '/';
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int has_project_markers(const char *dir) {
  char src[PATH_MAX];
  char plugins[PATH_MAX];
  int n1 = snprintf(src, sizeof(src), "%s/src", dir);
  int n2 = snprintf(plugins, sizeof(plugins), "%s/plugins", dir);
  return n1 > 0 && (size_t)n1 < sizeof(src) &&
         n2 > 0 && (size_t)n2 < sizeof(plugins) &&
         is_dir(src) && is_dir(plugins);
}

static int set_workdir(const char *path) {
  if (!is_absolute_path(path) || !is_dir(path))
    return 0;
  snprintf(g_workdir, sizeof(g_workdir), "%s", path);
  return 1;
}

static void dirname_in_place(char *path) {
  char *slash = strrchr(path, '/');
  if (!slash) {
    strcpy(path, ".");
  } else if (slash == path) {
    path[1] = '\0';
  } else {
    *slash = '\0';
  }
}

static int infer_workdir_from_binary(const char *argv0) {
  if (!argv0 || !argv0[0])
    return 0;

  char binary[PATH_MAX];
  if (is_absolute_path(argv0)) {
    snprintf(binary, sizeof(binary), "%s", argv0);
  } else if (strchr(argv0, '/')) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
      return 0;
    int n = snprintf(binary, sizeof(binary), "%s/%s", cwd, argv0);
    if (n < 0 || (size_t)n >= sizeof(binary))
      return 0;
  } else {
    return 0;
  }

  char real_binary[PATH_MAX];
  if (realpath(binary, real_binary))
    snprintf(binary, sizeof(binary), "%s", real_binary);

  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", binary);
  dirname_in_place(dir);

  if (has_project_markers(dir))
    return set_workdir(dir);

  char parent[PATH_MAX];
  snprintf(parent, sizeof(parent), "%s", dir);
  dirname_in_place(parent);
  if (has_project_markers(parent))
    return set_workdir(parent);

  return 0;
}

void app_workdir_init(const char *argv0) {
  if (g_workdir[0])
    return;

  const char *env = getenv("CAPSTAN_WORKDIR");
  if (set_workdir(env))
    return;

  env = getenv("CAPSTAN_WORKSPACE");
  if (set_workdir(env))
    return;

  if (infer_workdir_from_binary(argv0))
    return;

  env = getenv("PWD");
  if (set_workdir(env))
    return;

  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) && set_workdir(cwd))
    return;

  snprintf(g_workdir, sizeof(g_workdir), ".");
}

const char *app_workdir(void) {
  if (!g_workdir[0])
    app_workdir_init(NULL);
  return g_workdir;
}

int app_workdir_set(const char *path) { return set_workdir(path); }

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

int app_state_dir(char *buf, size_t buf_size) {
  const char *base = getenv("XDG_STATE_HOME");
  const char *home = getenv("HOME");
  if (buf_size == 0)
    return -1;

  int n;
  if (base && base[0]) {
    n = snprintf(buf, buf_size, "%s/%s", base, APP_CONFIG_DIR_NAME);
  } else {
    if (!home || !home[0])
      return -1;
    n = snprintf(buf, buf_size, "%s/.local/state/%s", home,
                 APP_CONFIG_DIR_NAME);
  }
  if (n < 0 || (size_t)n >= buf_size)
    return -1;
  return 0;
}

int app_state_path(char *buf, size_t buf_size, const char *relative_path) {
  char dir[512];
  if (app_state_dir(dir, sizeof(dir)) != 0)
    return -1;

  int n = snprintf(buf, buf_size, "%s/%s", dir, relative_path);
  if (n < 0 || (size_t)n >= buf_size)
    return -1;
  return 0;
}

int app_state_ensure_dir(void) {
  char dir[512];
  if (app_state_dir(dir, sizeof(dir)) != 0)
    return -1;

  const char *base = getenv("XDG_STATE_HOME");
  const char *home = getenv("HOME");
  if (base && base[0]) {
    mkdir(base, 0755);
  } else if (home && home[0]) {
    char local[512];
    int n = snprintf(local, sizeof(local), "%s/.local", home);
    if (n < 0 || (size_t)n >= sizeof(local))
      return -1;
    mkdir(local, 0755);
    n = snprintf(local, sizeof(local), "%s/.local/state", home);
    if (n < 0 || (size_t)n >= sizeof(local))
      return -1;
    mkdir(local, 0755);
  } else {
    return -1;
  }

  mkdir(dir, 0755);
  return 0;
}
