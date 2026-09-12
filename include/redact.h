#ifndef REDACT_H
#define REDACT_H

#include <stddef.h>

/* Shared classification for text assignments and structured argv options. */
int redact_sensitive_key(const char *key, size_t len);
char *redact_secrets_alloc(const char *input);

#endif
