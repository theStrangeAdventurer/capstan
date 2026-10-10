#include "permit_logic.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int permit_path_is_sensitive(const char *path) {
  if (!path) return 0;
  const char *name = strrchr(path, '/');
  name = name && name[1] ? name + 1 : path;
  char lower[PERMIT_MAX_TARGET];
  size_t n = strlen(name);
  if (n >= sizeof(lower)) return 1;
  for (size_t i = 0; i <= n; i++) lower[i] = (char)tolower((unsigned char)name[i]);
  if (!strcmp(lower, ".env") || !strncmp(lower, ".env.", 5) ||
      strstr(lower, "secret") || strstr(lower, "credential")) return 1;
  if (!strstr(lower, "token")) return 0;
  /* Token counting, parsing and design-token modules are ordinary source.
   * Keep data/config files, extensionless token stores and backup suffixes
   * sensitive. Explicit secret/credential names above still take priority. */
  const char *extension = strrchr(lower, '.');
  static const char *source_extensions[] = {
      ".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx",
      ".lua", ".py", ".js", ".jsx", ".ts", ".tsx", ".go", ".rs",
      ".java", ".kt", ".kts", ".swift", ".rb", ".php", ".cs",
      ".scala", ".ex", ".exs", ".erl", ".hrl", ".clj", ".cljs",
      ".sh", ".bash", ".zsh", ".md", ".rst", NULL};
  for (int i = 0; extension && source_extensions[i]; i++)
    if (!strcmp(extension, source_extensions[i])) return 0;
  return 1;
}

static const char *expand_home_pattern(const char *pattern, char *buf,
                                       size_t buf_size) {
  if (!pattern)
    return "";

  if (pattern[0] != '~' || (pattern[1] != '/' && pattern[1] != '\0'))
    return pattern;

  const char *home = getenv("HOME");
  if (!home || !home[0])
    return pattern;

  int n = snprintf(buf, buf_size, "%s%s", home, pattern + 1);
  if (n < 0 || (size_t)n >= buf_size)
    return pattern;
  return buf;
}

static int glob_match(const char *pattern, const char *target) {
  const char *star = NULL;
  const char *retry = NULL;

  while (*target) {
    if (*pattern == '*') {
      star = pattern++;
      retry = target;
    } else if (*pattern == '?' || *pattern == *target) {
      pattern++;
      target++;
    } else if (star) {
      pattern = star + 1;
      target = ++retry;
    } else {
      return 0;
    }
  }

  while (*pattern == '*')
    pattern++;
  return *pattern == '\0';
}

int permit_pattern_match(const char *pattern, const char *target) {
  if (!target)
    return 0;

  char expanded[PERMIT_MAX_TARGET];
  pattern = expand_home_pattern(pattern, expanded, sizeof(expanded));

  size_t plen = strlen(pattern);
  if (plen >= 2 && pattern[plen - 2] == '/' && pattern[plen - 1] == '*') {
    size_t dir_len = plen - 2;
    if (strlen(target) == dir_len && strncmp(target, pattern, dir_len) == 0)
      return 1;
  }
  if (strchr(pattern, '*') || strchr(pattern, '?'))
    return glob_match(pattern, target);
  return strcmp(pattern, target) == 0;
}

PermState permit_file_read_check(const char *workdir, const char *target) {
  if (!workdir || !workdir[0] || !target || !target[0])
    return PERM_ASK;

  char cwd[PERMIT_MAX_TARGET];
  int cwd_n = snprintf(cwd, sizeof(cwd), "%s", workdir);
  if (cwd_n < 0 || (size_t)cwd_n >= sizeof(cwd))
    return PERM_ASK;

  char full[PERMIT_MAX_TARGET * 2];
  if (target[0] == '/')
    snprintf(full, sizeof(full), "%s", target);
  else
    snprintf(full, sizeof(full), "%s/%s", cwd, target);

  char *parts[256];
  int n = 0;
  char *saveptr;
  char token_buf[PERMIT_MAX_TARGET * 2];
  strncpy(token_buf, full, sizeof(token_buf) - 1);
  token_buf[sizeof(token_buf) - 1] = '\0';

  char *token = strtok_r(token_buf, "/", &saveptr);
  while (token) {
    if (strcmp(token, ".") == 0) {
    } else if (strcmp(token, "..") == 0) {
      if (n > 0)
        n--;
    } else if (n < (int)(sizeof(parts) / sizeof(parts[0]))) {
      parts[n++] = token;
    } else {
      return PERM_ASK;
    }
    token = strtok_r(NULL, "/", &saveptr);
  }

  char resolved[PERMIT_MAX_TARGET];
  int pos = 0;
  if (target[0] == '/')
    resolved[pos++] = '/';
  for (int i = 0; i < n; i++) {
    if (i > 0 || target[0] != '/')
      resolved[pos++] = '/';
    int len = (int)strlen(parts[i]);
    if (pos + len >= (int)sizeof(resolved))
      return PERM_ASK;
    memcpy(resolved + pos, parts[i], len);
    pos += len;
  }
  resolved[pos] = '\0';
  if (pos == 0 && target[0] == '/')
    resolved[pos++] = '/', resolved[pos] = '\0';
  if (pos == 0)
    strcpy(resolved, cwd);

  size_t cwd_len = strlen(cwd);
  if (strncmp(resolved, cwd, cwd_len) == 0 &&
      (resolved[cwd_len] == '/' || resolved[cwd_len] == '\0'))
    return PERM_ALLOW;

  return PERM_ASK;
}

int permit_lua_escape_string(const char *input, char *out, size_t out_size) {
  if (!input || !out || out_size == 0)
    return 0;

  size_t pos = 0;
  for (const char *p = input; *p; p++) {
    const char *replacement = NULL;
    char escaped_byte[5];

    switch (*p) {
    case '\\':
      replacement = "\\\\";
      break;
    case '"':
      replacement = "\\\"";
      break;
    case '\n':
      replacement = "\\n";
      break;
    case '\r':
      replacement = "\\r";
      break;
    case '\t':
      replacement = "\\t";
      break;
    default:
      if ((unsigned char)*p < 0x20) {
        snprintf(escaped_byte, sizeof(escaped_byte), "\\%03u",
                 (unsigned char)*p);
        replacement = escaped_byte;
      }
      break;
    }

    if (replacement) {
      size_t len = strlen(replacement);
      if (pos + len >= out_size)
        return 0;
      memcpy(out + pos, replacement, len);
      pos += len;
    } else {
      if (pos + 1 >= out_size)
        return 0;
      out[pos++] = *p;
    }
  }

  out[pos] = '\0';
  return 1;
}
