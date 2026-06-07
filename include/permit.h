#ifndef PERMIT_H
#define PERMIT_H

#include <lua.h>

#define PERMIT_MAX_TARGET 4096
#define PERMIT_DEFAULT_SHELL_TIMEOUT 60
#define PERMIT_MAX_STDOUT (1024 * 1024)
#define PERMIT_MAX_STDERR (256 * 1024)

typedef enum { PERM_ASK, PERM_ALLOW, PERM_DENY } PermState;

typedef struct {
  char *tool;
  char *pattern;
  int allow;
} PermEntry;

typedef struct {
  PermEntry **items;
  size_t size;
  size_t capacity;
} PermEntries;

PermState permit_check(const char *tool, const char *target);
void permit_grant(const char *tool, const char *pattern, int allow);
void permit_load(const char *path);
void permit_save(const char *path);

void permit_init(lua_State *L);
void tools_init(lua_State *L);

const char *permit_config_dir(void);

#endif
