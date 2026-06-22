#ifndef PERMIT_LOGIC_H
#define PERMIT_LOGIC_H

#define PERMIT_MAX_TARGET 4096

typedef enum { PERM_ASK, PERM_ALLOW, PERM_DENY } PermState;

int permit_pattern_match(const char *pattern, const char *target);
PermState permit_file_read_check(const char *workdir, const char *target);

#endif
