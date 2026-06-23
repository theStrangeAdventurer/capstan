#ifndef PERMIT_LOGIC_H
#define PERMIT_LOGIC_H

#include <stddef.h>

#define PERMIT_MAX_TARGET 4096

typedef enum { PERM_ASK, PERM_ALLOW, PERM_DENY } PermState;

int permit_pattern_match(const char *pattern, const char *target);
PermState permit_file_read_check(const char *workdir, const char *target);
int permit_lua_escape_string(const char *input, char *out, size_t out_size);

#endif
