#include "redact.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int is_key_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

static int is_boundary(char c) {
  return c == '\0' || isspace((unsigned char)c) || c == ',' || c == ';' ||
         c == '}' || c == ']';
}

static void lower_copy(const char *src, size_t len, char *out, size_t out_size) {
  size_t n = len < out_size - 1 ? len : out_size - 1;
  for (size_t i = 0; i < n; i++)
    out[i] = (char)tolower((unsigned char)src[i]);
  out[n] = '\0';
}

static int contains_upper_token(const char *key, size_t len, const char *needle) {
  size_t needle_len = strlen(needle);
  if (needle_len == 0 || len < needle_len)
    return 0;
  for (size_t i = 0; i + needle_len <= len; i++) {
    size_t j = 0;
    for (; j < needle_len; j++) {
      if ((char)toupper((unsigned char)key[i + j]) != needle[j])
        break;
    }
    if (j == needle_len)
      return 1;
  }
  return 0;
}

static int sensitive_key(const char *key, size_t len) {
  char lower[128];
  lower_copy(key, len, lower, sizeof(lower));

  const char *exact[] = {
      "authorization",
      "proxy-authorization",
      "cookie",
      "set-cookie",
      "passwd",
  };
  for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
    if (strcmp(lower, exact[i]) == 0)
      return 1;
  }

  if (contains_upper_token(key, len, "TOKEN") ||
      contains_upper_token(key, len, "SECRET") ||
      contains_upper_token(key, len, "PASSWORD") ||
      contains_upper_token(key, len, "AUTH") ||
      contains_upper_token(key, len, "API_KEY"))
    return 1;

  return 0;
}

static int append_bytes(char **out, size_t *len, size_t *cap, const char *s,
                        size_t n) {
  if (*len + n + 1 > *cap) {
    size_t new_cap = *cap ? *cap : 128;
    while (*len + n + 1 > new_cap)
      new_cap *= 2;
    char *new_out = realloc(*out, new_cap);
    if (!new_out)
      return 0;
    *out = new_out;
    *cap = new_cap;
  }
  memcpy(*out + *len, s, n);
  *len += n;
  (*out)[*len] = '\0';
  return 1;
}

static int append_cstr(char **out, size_t *len, size_t *cap, const char *s) {
  return append_bytes(out, len, cap, s, strlen(s));
}

char *redact_secrets_alloc(const char *input) {
  if (!input)
    input = "";

  char *out = NULL;
  size_t out_len = 0, out_cap = 0;
  size_t i = 0;
  size_t n = strlen(input);

  while (i < n) {
    size_t start = i;
    int quoted_key = 0;
    if (input[i] == '"' || input[i] == '\'') {
      quoted_key = input[i];
      i++;
      start = i;
    }

    if (!isalpha((unsigned char)input[i]) && input[i] != '_') {
      if (!append_bytes(&out, &out_len, &out_cap, input + (quoted_key ? start - 1 : i), 1))
        goto oom;
      i = quoted_key ? start : i + 1;
      continue;
    }

    while (i < n && is_key_char(input[i]))
      i++;
    size_t key_end = i;

    if (quoted_key && i < n && input[i] == quoted_key)
      i++;
    else if (quoted_key) {
      if (!append_bytes(&out, &out_len, &out_cap, input + start - 1, key_end - start + 1))
        goto oom;
      continue;
    }

    size_t after_key = i;
    while (i < n && (input[i] == ' ' || input[i] == '\t'))
      i++;

    if (i >= n || (input[i] != ':' && input[i] != '=')) {
      if (!append_bytes(&out, &out_len, &out_cap,
                        input + (quoted_key ? start - 1 : start),
                        after_key - (quoted_key ? start - 1 : start)))
        goto oom;
      continue;
    }

    char sep = input[i++];
    if (!sensitive_key(input + start, key_end - start)) {
      if (!append_bytes(&out, &out_len, &out_cap,
                        input + (quoted_key ? start - 1 : start),
                        i - (quoted_key ? start - 1 : start)))
        goto oom;
      continue;
    }

    if (!append_bytes(&out, &out_len, &out_cap,
                      input + (quoted_key ? start - 1 : start),
                      i - (quoted_key ? start - 1 : start)))
      goto oom;
    while (i < n && (input[i] == ' ' || input[i] == '\t')) {
      if (!append_bytes(&out, &out_len, &out_cap, input + i, 1))
        goto oom;
      i++;
    }

    int value_quote = 0;
    if (i < n && (input[i] == '"' || input[i] == '\'')) {
      value_quote = input[i];
      if (!append_bytes(&out, &out_len, &out_cap, input + i, 1))
        goto oom;
      i++;
    }
    if (!append_cstr(&out, &out_len, &out_cap, "[REDACTED]"))
      goto oom;

    if (sep == ':' && !value_quote) {
      while (i < n && input[i] != '\n' && input[i] != '\r')
        i++;
    } else if (value_quote) {
      while (i < n && input[i] != value_quote)
        i++;
      if (i < n && input[i] == value_quote) {
        if (!append_bytes(&out, &out_len, &out_cap, input + i, 1))
          goto oom;
        i++;
      }
    } else {
      while (i < n && !is_boundary(input[i]))
        i++;
    }
  }

  if (!out)
    return strdup("");
  return out;

oom:
  free(out);
  return strdup("[REDACTION_FAILED]");
}
