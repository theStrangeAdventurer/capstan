#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oauth_store.h"
#include "munit.h"
#include <lauxlib.h>
#include <lualib.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void run(lua_State *L, const char *code) {
    if (luaL_dostring(L, code) != LUA_OK)
        munit_errorf("Lua assertion: %s", lua_tostring(L, -1));
}

static lua_State *new_state(const char *path) {
    lua_State *L = luaL_newstate();
    munit_assert_not_null(L);
    luaL_openlibs(L);
    oauth_store_init(L);
    munit_assert_int(lua_gettop(L), ==, 0);
    lua_pushstring(L, path);
    lua_setglobal(L, "path");
    run(L, "open = capstan.oauth.store_open; function bad(v,e) assert(v == nil and type(e)=='string') end");
    return L;
}

static void fixture(char *root, char *path, size_t size) {
    munit_assert_not_null(mkdtemp(root));
    snprintf(path, size, "%s/store", root);
}

static void cleanup(const char *root, const char *path) {
    char file[512];
    snprintf(file, sizeof(file), "%s/state.json", path); unlink(file);
    snprintf(file, sizeof(file), "%s/.lock", path); unlink(file);
    munit_assert_int(rmdir(path), ==, 0);
    munit_assert_int(rmdir(root), ==, 0);
}

static MunitResult normal(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    char root[] = "build/oauth-store-normal.XXXXXX", path[256], state[512];
    fixture(root, path, sizeof(path));
    lua_State *L = new_state(path);
    run(L, "a=assert(open(path)); b=assert(open(path)); bad(a:read()); bad(a:write('{}'));"
           "assert(a:try_lock()); assert(a:try_lock()); assert(b:try_lock()==false);"
           "assert(b:unlock()); assert(b:try_lock()==false);"
           "local v,e=a:read(); assert(v==nil and e==nil);"
           "assert(a:write('{\"a\":1}')); assert(a:read()=='{\"a\":1}');");
    snprintf(state, sizeof(state), "%s/state.json", path);
    int old = open(state, O_RDONLY);
    munit_assert_int(old, >=, 0);
    run(L, "assert(a:write('not executable JSON')); assert(a:read()=='not executable JSON');"
           "local s=string.rep('x',1048576); assert(a:write(s)); assert(a:read()==s);"
           "bad(a:write(s..'x')); assert(a:read()==s)");
    char buf[32] = {0};
    munit_assert_int(read(old, buf, sizeof(buf)), ==, 7);
    munit_assert_string_equal(buf, "{\"a\":1}");
    close(old);
    struct stat st;
    munit_assert_int(stat(path, &st), ==, 0);
    munit_assert_int(st.st_mode & 07777, ==, 0700);
    munit_assert_int(stat(state, &st), ==, 0);
    munit_assert_int(st.st_mode & 07777, ==, 0600);
    run(L, "assert(a:unlock()); assert(b:try_lock()); assert(a:close());"
           "c=assert(open(path)); assert(c:try_lock()==false);"
           "b=nil; collectgarbage('collect'); assert(c:try_lock());"
           "assert(c:close()); assert(c:close()); bad(c:try_lock()); bad(c:read())");
    lua_close(L);
    cleanup(root, path);
    return MUNIT_OK;
}

static MunitResult processes(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    char root[] = "build/oauth-store-process.XXXXXX", path[256];
    fixture(root, path, sizeof(path));
    lua_State *L = new_state(path);
    run(L, "a=assert(open(path)); assert(a:try_lock())");
    pid_t child = fork();
    munit_assert_int(child, >=, 0);
    if (!child) {
        /* Inherited userdata must not unlock the parent's open description. */
        run(L, "bad(a:unlock()); bad(a:read()); assert(a:close())");
        lua_State *other = new_state(path);
        run(other, "b=assert(open(path)); assert(b:try_lock()==false); assert(b:close())");
        lua_close(other);
        lua_close(L);
        _exit(0);
    }
    int status;
    munit_assert_int(waitpid(child, &status, 0), ==, child);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);
    run(L, "b=assert(open(path)); assert(b:try_lock()==false); assert(a:unlock())");
    child = fork();
    munit_assert_int(child, >=, 0);
    if (!child) {
        lua_State *other = new_state(path);
        run(other, "b=assert(open(path)); assert(b:try_lock()); assert(b:write('{}')); assert(b:close())");
        lua_close(other);
        _exit(0);
    }
    munit_assert_int(waitpid(child, &status, 0), ==, child);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);
    run(L, "assert(b:try_lock()); assert(b:read()=='{}')");
    lua_close(L);
    cleanup(root, path);
    return MUNIT_OK;
}

static MunitResult unsafe(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    char root[] = "build/oauth-store-unsafe.XXXXXX", path[256], state[512], linkpath[512], lock[512];
    fixture(root, path, sizeof(path));
    lua_State *L = new_state(path);
    run(L, "bad(open(path..'/missing/child')); bad(open(path..'\\0extra'));"
           "a=assert(open(path)); assert(a:try_lock()); assert(a:write('{}'))");
    snprintf(state, sizeof(state), "%s/state.json", path);
    snprintf(linkpath, sizeof(linkpath), "%s/link", root);
    snprintf(lock, sizeof(lock), "%s/.lock", path);
    munit_assert_int(chmod(path, 0755), ==, 0);
    run(L, "bad(open(path)); bad(a:read()); bad(a:write('{}'))");
    munit_assert_int(chmod(path, 0700), ==, 0);
    munit_assert_int(chmod(state, 0644), ==, 0);
    run(L, "bad(a:read()); bad(a:write('{}'))");
    munit_assert_int(chmod(state, 0600), ==, 0);
    munit_assert_int(link(state, linkpath), ==, 0);
    run(L, "bad(a:read()); bad(a:write('{}'))");
    munit_assert_int(unlink(linkpath), ==, 0);
    int fd = open(state, O_WRONLY);
    munit_assert_int(fd, >=, 0);
    munit_assert_int(ftruncate(fd, 1048577), ==, 0);
    close(fd);
    run(L, "bad(a:read()); assert(a:write('{}'))");
    munit_assert_int(unlink(state), ==, 0);
    munit_assert_int(symlink(".lock", state), ==, 0);
    run(L, "bad(a:read()); bad(a:write('{}'))");
    munit_assert_int(unlink(state), ==, 0);
    munit_assert_int(mkfifo(state, 0600), ==, 0);
    run(L, "bad(a:read()); bad(a:write('{}'))");
    munit_assert_int(unlink(state), ==, 0);
    munit_assert_int(symlink("store", linkpath), ==, 0);
    lua_pushstring(L, linkpath); lua_setglobal(L, "linkpath");
    run(L, "bad(open(linkpath)); bad(open(linkpath..'/child')); bad(open(path..'/../other'))");
    munit_assert_int(unlink(linkpath), ==, 0);
    munit_assert_int(chmod(lock, 0644), ==, 0);
    run(L, "bad(open(path)); bad(a:try_lock()); bad(a:read())");
    munit_assert_int(chmod(lock, 0600), ==, 0);
    munit_assert_int(link(lock, linkpath), ==, 0);
    run(L, "bad(open(path)); bad(a:try_lock())");
    munit_assert_int(unlink(linkpath), ==, 0);
    run(L, "assert(a:close())");
    munit_assert_int(unlink(lock), ==, 0);
    munit_assert_int(symlink("state.json", lock), ==, 0);
    run(L, "bad(open(path))");
    lua_close(L);
    cleanup(root, path);
    return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/normal-bounds-atomic-gc", normal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/process-locks", processes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/unsafe-paths", unsafe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite oauth_store_suite = {"/oauth-store", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};

#ifdef OAUTH_STORE_TEST_MAIN
int main(int argc, char **argv) {
    return munit_suite_main(&oauth_store_suite, NULL, argc, argv);
}
#endif
