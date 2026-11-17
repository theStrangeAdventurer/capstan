#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oauth_store.h"
#include <lauxlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_MT "capstan.oauth.store"
#define STORE_LIMIT (1024u * 1024u)

typedef struct {
    int parent, dir, lock, locked;
    pid_t pid;
    char *name;
} OAuthStore;

static int store_error(lua_State *L, const char *error) {
    lua_pushnil(L);
    lua_pushstring(L, error);
    return 2;
}

static int store_private(const struct stat *st, int directory) {
    return st->st_uid == geteuid() &&
        (st->st_mode & 07777) == (directory ? 0700 : 0600) &&
        (directory ? S_ISDIR(st->st_mode) :
         (S_ISREG(st->st_mode) && st->st_nlink == 1));
}

static int store_matches(int parent, const char *name, int fd, int directory) {
    struct stat a, b;
    return fstat(fd, &a) == 0 && store_private(&a, directory) &&
        fstatat(parent, name, &b, AT_SYMLINK_NOFOLLOW) == 0 &&
        store_private(&b, directory) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static int store_valid(OAuthStore *s) {
    return s->pid == getpid() && s->dir >= 0 && s->lock >= 0 &&
        store_matches(s->parent, s->name, s->dir, 1) &&
        store_matches(s->dir, ".lock", s->lock, 0);
}

static void store_dispose(OAuthStore *s) {
    /* close, not LOCK_UN: a forked child's close must not unlock its parent. */
    if (s->lock >= 0) close(s->lock);
    if (s->dir >= 0) close(s->dir);
    if (s->parent >= 0) close(s->parent);
    free(s->name);
    s->name = NULL;
    s->parent = s->dir = s->lock = -1;
    s->locked = 0;
}

static int l_store_close(lua_State *L) {
    OAuthStore *s = luaL_checkudata(L, 1, STORE_MT);
    store_dispose(s);
    lua_pushboolean(L, 1);
    return 1;
}

/* Walk each component so O_NOFOLLOW also protects parent components. */
static int store_directory(OAuthStore *s, const char *path, size_t len) {
    char *copy = malloc(len + 1), *part, *next;
    int fd;
    if (!copy) return 0;
    memcpy(copy, path, len + 1);
    fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    part = copy;
    while (*part == '/') part++;
    while (fd >= 0 && *part) {
        char *end = strchr(part, '/');
        if (end) {
            *end = '\0';
            next = end + 1;
            while (*next == '/') next++;
        } else next = part + strlen(part);
        if (!strcmp(part, "..")) break;
        if (!*next) {
            if (!strcmp(part, ".")) break;
            s->name = malloc(strlen(part) + 1);
            if (!s->name) break;
            strcpy(s->name, part);
            s->parent = fd;
            free(copy);
            if (mkdirat(fd, s->name, 0700) != 0 && errno != EEXIST) return 0;
            s->dir = openat(fd, s->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            return s->dir >= 0 && store_matches(fd, s->name, s->dir, 1);
        }
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = child;
        part = next;
    }
    if (fd >= 0) close(fd);
    free(copy);
    return 0;
}

static int l_store_open(lua_State *L) {
    size_t len;
    const char *path = luaL_checklstring(L, 1, &len);
    if (!len || memchr(path, '\0', len)) return store_error(L, "Invalid storage directory");
    OAuthStore *s = lua_newuserdatauv(L, sizeof(*s), 0);
    *s = (OAuthStore){.parent = -1, .dir = -1, .lock = -1, .pid = getpid()};
    luaL_setmetatable(L, STORE_MT);
    if (store_directory(s, path, len)) {
        s->lock = openat(s->dir, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW |
                         O_NONBLOCK | O_CLOEXEC, 0600);
        if (store_valid(s)) return 1;
    }
    store_dispose(s);
    return store_error(L, "Unsafe or unavailable storage");
}

static int l_store_try_lock(lua_State *L) {
    OAuthStore *s = luaL_checkudata(L, 1, STORE_MT);
    if (!store_valid(s)) return store_error(L, "Unsafe or closed storage");
    /* flock is per open description, unlike process-wide POSIX record locks. */
    if (!s->locked && flock(s->lock, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            lua_pushboolean(L, 0);
            return 1;
        }
        return store_error(L, "Storage lock failed");
    }
    s->locked = 1;
    if (!store_valid(s)) {
        flock(s->lock, LOCK_UN);
        s->locked = 0;
        return store_error(L, "Unsafe storage lock");
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_store_unlock(lua_State *L) {
    OAuthStore *s = luaL_checkudata(L, 1, STORE_MT);
    if (s->pid != getpid() || s->lock < 0) return store_error(L, "Closed storage");
    if (s->locked && flock(s->lock, LOCK_UN) != 0) return store_error(L, "Storage unlock failed");
    s->locked = 0;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_store_read(lua_State *L) {
    OAuthStore *s = luaL_checkudata(L, 1, STORE_MT);
    if (!s->locked || !store_valid(s)) return store_error(L, "Storage lock required");
    int fd = openat(s->dir, "state.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) { lua_pushnil(L); return 1; }
        return store_error(L, "Storage read failed");
    }
    struct stat st;
    if (!store_matches(s->dir, "state.json", fd, 0) || fstat(fd, &st) != 0 ||
        st.st_size < 0 || st.st_size > STORE_LIMIT) {
        close(fd);
        return store_error(L, "Unsafe or oversized state");
    }
    char *buf = malloc(STORE_LIMIT + 1);
    if (!buf) { close(fd); return store_error(L, "Storage allocation failed"); }
    size_t used = 0;
    int ok = 1;
    while (used <= STORE_LIMIT) {
        ssize_t n = read(fd, buf + used, STORE_LIMIT + 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = 0; break; }
        if (!n) break;
        used += (size_t)n;
    }
    ok = ok && used <= STORE_LIMIT && store_matches(s->dir, "state.json", fd, 0) && store_valid(s);
    close(fd);
    if (ok) lua_pushlstring(L, buf, used);
    /* Do not retain an extra native copy of credentials. */
    volatile char *wipe = buf;
    for (size_t i = 0; i < used; i++) wipe[i] = 0;
    free(buf);
    return ok ? 1 : store_error(L, "Storage read failed");
}

static int store_state_safe(OAuthStore *s) {
    struct stat st;
    if (fstatat(s->dir, "state.json", &st, AT_SYMLINK_NOFOLLOW) == 0)
        return store_private(&st, 0);
    return errno == ENOENT;
}

static int l_store_write(lua_State *L) {
    OAuthStore *s = luaL_checkudata(L, 1, STORE_MT);
    size_t len;
    const char *json = luaL_checklstring(L, 2, &len);
    if (len > STORE_LIMIT) return store_error(L, "State exceeds storage limit");
    if (!s->locked || !store_valid(s)) return store_error(L, "Storage lock required");
    if (!store_state_safe(s)) return store_error(L, "Unsafe state file");
    char name[80];
    int fd = -1;
    /* O_EXCL, not unpredictability, makes collisions safe. Never remove a collision. */
    for (unsigned i = 0; i < 1024; i++) {
        snprintf(name, sizeof(name), ".state.%ld.%u.tmp", (long)getpid(), i);
        fd = openat(s->dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return store_error(L, "Storage temporary file failed");
    int ok = store_matches(s->dir, name, fd, 0);
    size_t done = 0;
    while (ok && done < len) {
        ssize_t n = write(fd, json + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        done += (size_t)n;
    }
    ok = ok && fsync(fd) == 0 && store_valid(s) && store_state_safe(s) &&
        store_matches(s->dir, name, fd, 0);
    if (close(fd) != 0) ok = 0;
    if (ok && renameat(s->dir, name, s->dir, "state.json") == 0) {
        if (fsync(s->dir) != 0) return store_error(L, "Storage durability failed");
        lua_pushboolean(L, 1);
        return 1;
    }
    unlinkat(s->dir, name, 0);
    return store_error(L, "Storage write failed");
}

void oauth_store_init(lua_State *L) {
    static const luaL_Reg methods[] = {
        {"try_lock", l_store_try_lock}, {"unlock", l_store_unlock},
        {"read", l_store_read}, {"write", l_store_write},
        {"close", l_store_close}, {"__gc", l_store_close}, {NULL, NULL}
    };
    luaL_newmetatable(L, STORE_MT);
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "oauth store");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    lua_getglobal(L, "capstan");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1); lua_newtable(L);
        lua_pushvalue(L, -1); lua_setglobal(L, "capstan");
    }
    lua_getfield(L, -1, "oauth");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1); lua_newtable(L);
        lua_pushvalue(L, -1); lua_setfield(L, -3, "oauth");
    }
    lua_pushcfunction(L, l_store_open);
    lua_setfield(L, -2, "store_open");
    lua_pop(L, 2);
}
