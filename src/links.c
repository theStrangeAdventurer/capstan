#include "links.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINK_TARGET_MAX 4096
#define LINK_TARGET_LIMIT (LINK_TARGET_MAX - 1)

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Characters that end a token: whitespace and markup around it. */
static int is_delimiter(char c) {
  return is_space(c) || c == '<' || c == '>' || c == '"' || c == '\'' ||
         c == '`' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
         c == '}';
}

static int ends_sentence(char c) {
  return c == '.' || c == ',' || c == ';' || c == ':' || c == '!';
}

static int has_prefix_ci(const char *text, size_t length, const char *prefix) {
  size_t size = strlen(prefix);
  if (length < size) return 0;
  for (size_t i = 0; i < size; i++) {
    char a = text[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != b) return 0;
  }
  return 1;
}

static int looks_like_path(const char *token, size_t length) {
  if (length < 2) return 0;
  for (size_t i = 0; i + 2 < length; i++) {
    if (token[i] == ':' && token[i + 1] == '/' && token[i + 2] == '/') return 0;
  }
  const char *slash = memchr(token, '/', length);
  if (!slash) return 0;
  if (token[0] == '/' || token[0] == '~') return 1;
  if (length > 2 && token[0] == '.' && (token[1] == '/' || token[1] == '.'))
    return 1;
  /* Relative path: the last component carries an extension (src/tui.c). */
  const char *last = token;
  for (size_t i = 0; i < length; i++)
    if (token[i] == '/') last = token + i + 1;
  for (const char *p = last; p < token + length; p++)
    if (*p == '.') return 1;
  return 0;
}

int links_scan(const char *text, size_t length, LinkSpan *spans, int max) {
  if (!text || !spans || max <= 0) return 0;
  int count = 0;
  size_t i = 0;
  while (i < length && count < max) {
    if (has_prefix_ci(text + i, length - i, "http://") ||
        has_prefix_ci(text + i, length - i, "https://")) {
      size_t end = i;
      while (end < length && !is_delimiter(text[end])) end++;
      while (end > i && ends_sentence(text[end - 1])) end--;
      if (end > i + 8) {
        spans[count].start = i;
        spans[count].end = end;
        spans[count].kind = LINK_URL;
        count++;
      }
      i = end > i ? end : i + 1;
      continue;
    }
    if (is_delimiter(text[i])) {
      i++;
      continue;
    }
    size_t end = i;
    while (end < length && !is_delimiter(text[end])) end++;
    size_t trimmed = end;
    while (trimmed > i && ends_sentence(text[trimmed - 1])) trimmed--;
    if (looks_like_path(text + i, trimmed - i)) {
      spans[count].start = i;
      spans[count].end = trimmed;
      spans[count].kind = LINK_PATH;
      count++;
    }
    i = end;
  }
  return count;
}

int links_allowed(const LinkSpan *span, int literal) {
  if (!span) return 0;
  return span->kind == LINK_URL || !literal;
}

int links_find(const LinkSpan *spans, int count, size_t offset) {
  if (!spans) return -1;
  for (int i = 0; i < count; i++)
    if (offset >= spans[i].start && offset < spans[i].end) return i;
  return -1;
}

static void set_error(char *error, size_t error_size, const char *message) {
  if (!error || !error_size) return;
  snprintf(error, error_size, "%s", message);
}

static void detach_child(void) {
  int null_fd = open("/dev/null", O_WRONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
  }
  null_fd = open("/dev/null", O_RDONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDIN_FILENO);
    close(null_fd);
  }
}

/* Launch without a shell and without a zombie: the intermediate child exits at
 * once, so the opener is reparented and owns its own window. */
static int launch(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    pid_t grandchild = fork();
    if (grandchild != 0) _exit(grandchild < 0);
    setsid();
    detach_child();
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  pid_t result;
  do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
  return result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Opens text that was clicked as a whole: a URL or a path. */
int links_open_text(const char *text, const char *base_dir, char *error,
                    size_t error_size) {
  if (!text) return 0;
  size_t length = strlen(text);
  LinkSpan spans[4];
  int count = links_scan(text, length, spans, 4);
  int link = links_find(spans, count, 0);
  if (link < 0) {
    set_error(error, error_size, "Nothing to open");
    return 0;
  }
  return links_open(text + spans[link].start, spans[link].end - spans[link].start,
                    spans[link].kind, base_dir, error, error_size);
}

int links_open(const char *text, size_t length, int kind, const char *base_dir,
               char *error, size_t error_size) {
  if (!text || !length || length >= LINK_TARGET_MAX) {
    set_error(error, error_size, "Link too long");
    return 0;
  }
  char target[LINK_TARGET_MAX];
  memcpy(target, text, length);
  target[length] = '\0';

  if (kind == LINK_URL) {
    if (!has_prefix_ci(target, length, "http://") &&
        !has_prefix_ci(target, length, "https://")) {
      set_error(error, error_size, "Not a web link");
      return 0;
    }
#ifdef __APPLE__
    char *const command[] = {"open", target, NULL};
#else
    char *const command[] = {"xdg-open", target, NULL};
#endif
    if (!launch(command)) {
      set_error(error, error_size, "Cannot open browser");
      return 0;
    }
    return 1;
  }

  char resolved[LINK_TARGET_MAX + PATH_MAX];
  if (target[0] == '~' && (target[1] == '\0' || target[1] == '/')) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
      set_error(error, error_size, "No home directory");
      return 0;
    }
    if (snprintf(resolved, sizeof(resolved), "%s%s", home, target + 1) >=
        (int)sizeof(resolved)) {
      set_error(error, error_size, "Link too long");
      return 0;
    }
  } else if (target[0] != '/' && base_dir && *base_dir) {
    if (snprintf(resolved, sizeof(resolved), "%s/%s", base_dir, target) >=
        (int)sizeof(resolved)) {
      set_error(error, error_size, "Link too long");
      return 0;
    }
  } else {
    memcpy(resolved, target, length + 1);
  }

  struct stat info;
  if (stat(resolved, &info) != 0) {
    set_error(error, error_size, "Path not found");
    return 0;
  }

  char parent[LINK_TARGET_MAX + PATH_MAX];
  snprintf(parent, sizeof(parent), "%s", resolved);
  char *slash = strrchr(parent, '/');
  if (slash && slash != parent) *slash = '\0';

#ifdef __APPLE__
  /* Files open in their folder, directories open themselves (Finder). */
  char *const reveal[] = {"open", "-R", resolved, NULL};
  char *const direct[] = {"open", resolved, NULL};
  int ok = launch(S_ISDIR(info.st_mode) ? direct : reveal);
#else
  char *const command[] = {"xdg-open", S_ISDIR(info.st_mode) ? resolved : parent,
                           NULL};
  int ok = launch(command);
#endif
  if (!ok) {
    set_error(error, error_size, "Cannot open file manager");
    return 0;
  }
  return 1;
}
