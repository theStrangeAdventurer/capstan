#include "permit_logic.h"
#include <stdio.h>
#include <string.h>

int permit_pattern_match(const char *pattern, const char *target) {
  size_t plen = strlen(pattern);
  if (plen >= 2 && pattern[plen - 2] == ' ' && pattern[plen - 1] == '*') {
    size_t prefix_len = plen - 2;
    return strncmp(target, pattern, prefix_len) == 0;
  }
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
