/* Descriptor-relative APIs require POSIX.1-2008 despite the project default. */
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "review_snapshot.h"
#include "permit.h"
#include <lauxlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RS_BYTES (4 * 1024 * 1024)
#define RS_ENTRIES 20000
#define RS_DEPTH 128

static int failure(lua_State *L, const char *reason) {
  lua_pushnil(L); lua_pushstring(L, reason); return 2;
}
static int sensitive(const char *name) {
  char p[PATH_MAX];
  size_t n = strlen(name);
  if (n >= sizeof(p)) return 1;
  for (size_t i = 0; i <= n; i++) p[i] = (char)tolower((unsigned char)name[i]);
  return !strncmp(p, ".env", 4) || !strncmp(p, ".zshenv", 7) ||
    strstr(p, "secret") || strstr(p, "token") || strstr(p, "credential") ||
    !strcmp(p, ".ssh") || !strcmp(p, ".aws") || !strcmp(p, ".gnupg") ||
    !strcmp(p, ".netrc") || !strcmp(p, ".npmrc") || !strcmp(p, ".bashrc") ||
    !strcmp(p, ".bash_profile") || !strcmp(p, ".profile") || !strcmp(p, ".zshrc") ||
    !strcmp(p, ".git") || !strcmp(p, ".hg") ||
    (n >= 4 && (!strcmp(p+n-4, ".pem") || !strcmp(p+n-4, ".key")));
}
static int valid(const char *path, int absolute) {
  if (!path[0] || (path[0] == '/') != absolute || strlen(path) >= PATH_MAX ||
      strchr(path, '\\')) return 0;
  if (absolute && !strcmp(path, "/")) return 1;
  const char *p = path + absolute;
  while (*p) {
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t)(end-p) : strlen(p);
    if (!n || (n == 1 && p[0] == '.') || (n == 2 && !strncmp(p, "..", 2))) return 0;
    char part[PATH_MAX]; memcpy(part, p, n); part[n] = 0;
    if (sensitive(part)) return 0;
    if (!end) return 1;
    p = end+1;
    if (!*p) return 0;
  }
  return 0;
}
/* No realpath/open(path) shortcut: even root ancestors must not be links. */
static int descend(int base, const char *path, int final_flags) {
  int fd = dup(base);
  if (fd < 0) return -1;
  const char *p = path;
  while (*p) {
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t)(end-p) : strlen(p);
    char part[PATH_MAX]; memcpy(part, p, n); part[n] = 0;
    int next = openat(fd, part, O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                     (end ? O_RDONLY | O_DIRECTORY : final_flags));
    int saved = errno; close(fd); errno = saved;
    if (next < 0) return -1;
    fd = next;
    if (!end) break;
    p = end+1;
  }
  return fd;
}
static int root_open(const char *root) {
  if (!valid(root, 1)) { errno = EINVAL; return -1; }
  int base = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (base < 0) return -1;
  int fd = descend(base, root+1, O_RDONLY | O_DIRECTORY);
  close(base); return fd;
}
static const char *string_arg(lua_State *L, int i) {
  size_t n;
  const char *s = luaL_checklstring(L, i, &n);
  return strlen(s) == n ? s : NULL;
}
static int l_snapshot_read(lua_State *L) {
  const char *root = string_arg(L, 1), *path = string_arg(L, 2);
  lua_Integer limit = luaL_checkinteger(L, 3);
  if (!root || !path || !valid(root, 1) || !valid(path, 0) || limit < 0 || limit > RS_BYTES)
    return failure(L, "unsafe snapshot path or byte limit");
  char full[PATH_MAX];
  int n = snprintf(full, sizeof(full), "%s%s%s", root, strcmp(root,"/") ? "/" : "", path);
  if (n < 0 || (size_t)n >= sizeof(full))
    return failure(L, "snapshot read not authorized");
  PermState permission = permit_check("file_read", full);
  if (permission == PERM_DENY)
    return failure(L, "snapshot read not authorized");
  /* Internal callback carries the captured run scope, never model arguments.
   * Recheck it per read; explicit native denies and path exclusions always win. */
  if (lua_isfunction(L, 4)) {
    lua_pushvalue(L, 4);
    lua_pushstring(L, full);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK)
      return failure(L, "snapshot authorization query failed");
    int allowed = lua_isboolean(L, -1) && lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!allowed) return failure(L, "snapshot read not authorized");
  } else if (permission != PERM_ALLOW) {
    return failure(L, "snapshot read not authorized");
  }
  int base = root_open(root);
  if (base < 0) return failure(L, "unsafe snapshot root");
  int fd = descend(base, path, O_RDONLY);
  close(base);
  if (fd < 0) return failure(L, "snapshot file unavailable");
  struct stat before, after;
  if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 || before.st_size > limit) {
    close(fd); return failure(L, "snapshot requires bounded regular file");
  }
  size_t size = (size_t)before.st_size, used = 0;
  char *buf = malloc(size+1);
  if (!buf) { close(fd); return failure(L, "snapshot allocation failed"); }
  int ok = 1;
  while (used <= size) {
    ssize_t got = read(fd, buf+used, size+1-used);
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) { ok = 0; break; }
    if (!got) break;
    used += (size_t)got;
  }
  if (fstat(fd, &after) || used != size || before.st_size != after.st_size ||
      before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime) ok = 0;
  close(fd);
  if (ok) lua_pushlstring(L, buf, size);
  free(buf);
  return ok ? 1 : failure(L, "snapshot file changed or read failed");
}
static int entry(lua_State *L, const char *path, mode_t mode, int *count) {
  const char *kind = S_ISREG(mode) ? "file" : S_ISDIR(mode) ? "directory" : S_ISLNK(mode) ? "symlink" : NULL;
  if (!kind || ++*count > RS_ENTRIES) return 0;
  lua_newtable(L);
  lua_pushstring(L, path); lua_setfield(L, -2, "path");
  lua_pushstring(L, kind); lua_setfield(L, -2, "kind");
  lua_pushboolean(L, 0); lua_setfield(L, -2, "ignored");
  lua_rawseti(L, -2, *count); return 1;
}
static int walk(lua_State *L, int fd, const char *prefix, int depth, int *count) {
  if (depth > RS_DEPTH) return 0;
  int copy = dup(fd);
  if (copy < 0) return 0;
  DIR *dir = fdopendir(copy);
  if (!dir) { close(copy); return 0; }
  int ok = 1;
  for (;;) {
    errno = 0;
    struct dirent *de = readdir(dir);
    if (!de) { if (errno) ok = 0; break; }
    if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..") || sensitive(de->d_name)) continue;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", de->d_name);
    struct stat st;
    if (n < 0 || (size_t)n >= sizeof(path) || !valid(path,0) ||
        fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) || !entry(L,path,st.st_mode,count)) { ok = 0; break; }
    if (S_ISDIR(st.st_mode)) {
      int child = openat(fd,de->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
      if (child < 0) { ok = 0; break; }
      ok = walk(L,child,path,depth+1,count); close(child);
      if (!ok) break;
    }
  }
  closedir(dir); return ok;
}
/* Detect enclosing repositories using directory metadata only. An unusable Git
 * repository is an error, never permission to fall back to an unfiltered walk. */
static int git_present(int root) {
  int fd = dup(root);
  if (fd < 0) return -1;
  int result = -1;
  for (int depth = 0; depth < RS_DEPTH; depth++) {
    struct stat st, parent;
    if (!fstatat(fd,".git",&st,AT_SYMLINK_NOFOLLOW)) { result = 1; break; }
    if (errno != ENOENT || fstat(fd,&st)) break;
    int up = openat(fd,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (up < 0) break;
    if (fstat(up,&parent)) { close(up); break; }
    if (st.st_dev == parent.st_dev && st.st_ino == parent.st_ino) { close(up); result = 0; break; }
    close(fd); fd = up;
  }
  close(fd); return result;
}
/* Git supplies names only. No shell, hooks, pagers, inherited Git overrides or
 * user/system config. Output and wall time are bounded; errors fail closed. */
static char *git_names(int root, size_t *length) {
  int pipes[2];
  if (pipe(pipes)) return NULL;
  pid_t pid = fork();
  if (!pid) {
    close(pipes[0]);
    int nullfd = open("/dev/null",O_RDWR);
    if (nullfd < 0 || fchdir(root) || dup2(pipes[1],STDOUT_FILENO) < 0 ||
        dup2(nullfd,STDIN_FILENO) < 0 || dup2(nullfd,STDERR_FILENO) < 0) _exit(127);
    close(nullfd); close(pipes[1]);
    char *const argv[] = {"git","-c","core.fsmonitor=false","ls-files","--cached","--others","--exclude-standard","-z",NULL};
    char *const env[] = {"PATH=/usr/bin:/bin","GIT_CONFIG_NOSYSTEM=1","GIT_CONFIG_GLOBAL=/dev/null","GIT_TERMINAL_PROMPT=0","LC_ALL=C",NULL};
    execve("/usr/bin/git",argv,env); _exit(127);
  }
  close(pipes[1]);
  if (pid < 0) { close(pipes[0]); return NULL; }
  char *buf = malloc(RS_BYTES+1);
  size_t used = 0;
  int ok = buf != NULL, done = 0, status = 0;
  time_t deadline = time(NULL)+5;
  while (ok && !done && time(NULL) < deadline) {
    struct pollfd p = {pipes[0],POLLIN,0};
    int ready = poll(&p,1,100);
    if (ready < 0) { if (errno == EINTR) continue; ok = 0; break; }
    if (!ready) continue;
    ssize_t n = read(pipes[0],buf+used,RS_BYTES+1-used);
    if (n < 0) { if (errno == EINTR) continue; ok = 0; break; }
    if (!n) { done = 1; break; }
    used += (size_t)n;
    if (used > RS_BYTES) ok = 0;
  }
  close(pipes[0]);
  if (!done || !ok) kill(pid,SIGKILL);
  /* Even after closing stdout a child must not leave this call unbounded. */
  for (;;) {
    pid_t waited = waitpid(pid,&status,WNOHANG);
    if (waited == pid) break;
    if (waited < 0 && errno != EINTR) { ok = 0; break; }
    if (time(NULL) >= deadline) { kill(pid,SIGKILL); ok = 0; }
    struct timespec pause = {0,10000000}; nanosleep(&pause,NULL);
  }
  if (!ok || !done || !WIFEXITED(status) || WEXITSTATUS(status) || (used && buf[used-1])) { free(buf); return NULL; }
  *length = used; return buf;
}
static int l_snapshot_list(lua_State *L) {
  const char *root = string_arg(L,1);
  int fd = root ? root_open(root) : -1;
  if (fd < 0) return failure(L,"unsafe snapshot root");
  int git = git_present(fd), count = 0, ok = git >= 0;
  lua_newtable(L);
  if (git == 0) ok = walk(L,fd,"",0,&count);
  else if (git > 0) {
    size_t length = 0;
    char *names = git_names(fd,&length);
    ok = names != NULL;
    /* Git can report duplicate names for unmerged index stages. */
    lua_newtable(L);
    int seen = lua_gettop(L);
    for (size_t i = 0; ok && i < length; ) {
      char *path = names+i;
      size_t size = strlen(path); i += size+1;
      /* ls-files collapses unregistered nested repositories to "path/".
       * Their contents were not enumerated: never silently omit them or walk
       * across the repository boundary. Ignored paths are excluded by Git;
       * registered gitlinks are returned without a trailing slash. */
      int collapsed = size && path[size-1] == '/';
      if (collapsed) path[size-1] = 0;
      if (!valid(path,0)) continue; /* Includes sensitive names: never opened. */
      if (collapsed) { ok = 0; break; }
      lua_getfield(L,seen,path); int duplicate = lua_toboolean(L,-1); lua_pop(L,1);
      if (duplicate) continue;
      lua_pushboolean(L,1); lua_setfield(L,seen,path);
      char parent[PATH_MAX]; snprintf(parent,sizeof(parent),"%s",path);
      char *slash = strrchr(parent,'/');
      int dir = dup(fd); const char *name = path;
      if (slash) { *slash = 0; close(dir); dir = descend(fd,parent,O_RDONLY|O_DIRECTORY); name = slash+1; }
      if (dir < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) continue;
        ok = 0; break;
      }
      struct stat st;
      int result = fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW), saved = errno;
      close(dir);
      if (result) { if (saved == ENOENT) continue; ok = 0; break; }
      lua_pushvalue(L,seen-1);
      ok = entry(L,path,st.st_mode,&count);
      lua_pop(L,1);
      /* Submodule directories are boundaries, not recursive snapshots. */
    }
    lua_pop(L,1); free(names);
  }
  close(fd);
  if (!ok) { lua_pop(L,1); return failure(L,"snapshot enumeration incomplete"); }
  return 1;
}
void review_snapshot_init(lua_State *L) {
  lua_pushcfunction(L,l_snapshot_list); lua_setfield(L,-2,"review_snapshot_list");
  lua_pushcfunction(L,l_snapshot_read); lua_setfield(L,-2,"review_snapshot_read");
}
