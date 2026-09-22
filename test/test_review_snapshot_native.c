/* Standalone native adapter regression test; permit stub models a nonprompting
 * authorization decision, while all filesystem/Git operations are real. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "review_snapshot.h"
#include "permit.h"
#include <lauxlib.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static PermState permission = PERM_ALLOW;
PermState permit_check(const char *tool, const char *target) {
  assert(!strcmp(tool,"file_read")); assert(target[0] == '/');
  return permission;
}
static void put(const char *path, const char *bytes, size_t size) {
  int fd = open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);
  assert(fd >= 0); assert(write(fd,bytes,size) == (ssize_t)size); assert(!close(fd));
}
static void git(const char *arg, const char *path) {
  pid_t pid = fork(); assert(pid >= 0);
  if (!pid) {
    int index = !strcmp(arg,"update-index");
    char *const argv[] = {"git",(char *)arg,index ? "--add" : (char *)path,
      index ? "--cacheinfo" : NULL,index ? (char *)path : NULL,NULL};
    char *const env[] = {"PATH=/usr/bin:/bin","GIT_CONFIG_NOSYSTEM=1","GIT_CONFIG_GLOBAL=/dev/null",NULL};
    execve("/usr/bin/git",argv,env); _exit(127);
  }
  int status; assert(waitpid(pid,&status,0) == pid);
  assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void call(lua_State *L, const char *method, const char *root, const char *path, int limit) {
  lua_settop(L,0); lua_getglobal(L,"tools"); lua_getfield(L,-1,method); lua_remove(L,-2);
  lua_pushstring(L,root);
  int args = 1;
  if (path) { lua_pushstring(L,path); lua_pushinteger(L,limit); args = 3; }
  assert(lua_pcall(L,args,LUA_MULTRET,0) == LUA_OK);
}
static int grant_read(lua_State *L) {
  assert(lua_isstring(L, 1)); lua_pushboolean(L, 1); return 1;
}
static int deny_read(lua_State *L) {
  lua_pushboolean(L, 0); return 1;
}
static int broken_read(lua_State *L) { return luaL_error(L, "scope unavailable"); }
static void scoped_read(lua_State *L, const char *root, lua_CFunction check, int allowed) {
  lua_settop(L, 0); lua_getglobal(L, "tools");
  lua_getfield(L, -1, "review_snapshot_read"); lua_remove(L, -2);
  lua_pushstring(L, root); lua_pushstring(L, "plain"); lua_pushinteger(L, 4);
  lua_pushcfunction(L, check);
  assert(lua_pcall(L, 4, LUA_MULTRET, 0) == LUA_OK);
  assert(allowed ? lua_type(L, 1) == LUA_TSTRING : lua_isnil(L, 1));
}
static int has(lua_State *L, const char *path, const char *kind) {
  assert(lua_istable(L,1));
  for (size_t i = 1; i <= lua_rawlen(L,1); i++) {
    lua_rawgeti(L,1,(lua_Integer)i); lua_getfield(L,-1,"path");
    int match = !strcmp(lua_tostring(L,-1),path); lua_pop(L,1);
    if (match) {
      lua_getfield(L,-1,"kind"); assert(!strcmp(lua_tostring(L,-1),kind)); lua_pop(L,1);
      lua_getfield(L,-1,"ignored"); assert(lua_isboolean(L,-1)); assert(!lua_toboolean(L,-1)); lua_pop(L,1);
    }
    lua_pop(L,1); if (match) return 1;
  }
  return 0;
}
static void remove_tree(const char *path) {
  struct stat st; assert(!lstat(path,&st));
  if (!S_ISDIR(st.st_mode)) { assert(!unlink(path)); return; }
  DIR *dir = opendir(path); assert(dir);
  struct dirent *de;
  while ((de = readdir(dir))) {
    if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
    char child[4096]; assert(snprintf(child,sizeof(child),"%s/%s",path,de->d_name) < (int)sizeof(child));
    remove_tree(child);
  }
  closedir(dir); assert(!rmdir(path));
}
int main(void) {
  char cwd[4096], root[4096], template[] = "/tmp/capstan-review-native-XXXXXX";
  assert(getcwd(cwd,sizeof(cwd))); assert(mkdtemp(template)); assert(realpath(template,root));
  assert(!chdir(root));
  lua_State *L = luaL_newstate(); assert(L);
  lua_newtable(L); review_snapshot_init(L); lua_setglobal(L,"tools");
  put("plain","a\0b\n",4); assert(!mkdir("dir",0700)); put("dir/nested","yes",3);
  assert(!symlink("plain","link")); assert(!symlink("dir","alias"));
  /* Empty sensitive fixtures only: their contents are never read. */
  put(".env", "",0); put("credentials.json","",0); assert(!mkdir(".ssh",0700));
  call(L,"review_snapshot_list",root,NULL,0);
  assert(has(L,"plain","file") && has(L,"dir/nested","file") && has(L,"link","symlink"));
  assert(!has(L,".env","file") && !has(L,"credentials.json","file"));
  call(L,"review_snapshot_read",root,"plain",4);
  size_t n; const char *bytes = lua_tolstring(L,1,&n); assert(bytes && n == 4 && !memcmp(bytes,"a\0b\n",4));
  const char *bad[] = {"plain","link","alias/nested","dir","../plain",".env","credentials.json",".ssh/id_rsa","/plain","dir//nested","dir/../plain",NULL};
  for (int i = 0; bad[i]; i++) {
    call(L,"review_snapshot_read",root,bad[i],3); assert(lua_isnil(L,1));
  }
  assert(!mkfifo("fifo",0600));
  call(L,"review_snapshot_read",root,"fifo",100); assert(lua_isnil(L,1));
  call(L,"review_snapshot_list",root,NULL,0); assert(lua_isnil(L,1)); assert(!unlink("fifo"));
  permission = PERM_ASK;
  call(L,"review_snapshot_read",root,"plain",4); assert(lua_isnil(L,1));
  scoped_read(L, root, grant_read, 1);
  scoped_read(L, root, deny_read, 0);
  scoped_read(L, root, broken_read, 0);
  permission = PERM_DENY;
  scoped_read(L, root, grant_read, 0);
  call(L,"review_snapshot_read",root,"plain",4); assert(lua_isnil(L,1)); permission = PERM_ALLOW;
  scoped_read(L, root, deny_read, 0);
  scoped_read(L, root, grant_read, 1);
  char unsafe[4096]; assert(snprintf(unsafe,sizeof(unsafe),"%s/alias",root) < (int)sizeof(unsafe));
  call(L,"review_snapshot_read",unsafe,"nested",4); assert(lua_isnil(L,1));
  call(L,"review_snapshot_list",unsafe,NULL,0); assert(lua_isnil(L,1));
  git("init","-q");
  put(".gitignore","ignored/\n*.skip\n",16); assert(!mkdir("ignored",0700)); put("ignored/data","",0); put("a.skip","",0);
  git("add","plain"); git("add","dir/nested");
  call(L,"review_snapshot_list",root,NULL,0);
  assert(has(L,"plain","file") && has(L,"dir/nested","file"));
  assert(!has(L,"a.skip","file") && !has(L,"ignored/data","file") && !has(L,".git","directory"));
  assert(!rename("plain","renamed")); assert(!unlink("dir/nested")); put("added","new",3);
  call(L,"review_snapshot_list",root,NULL,0);
  assert(!has(L,"plain","file") && !has(L,"dir/nested","file"));
  assert(has(L,"renamed","file") && has(L,"added","file"));
  /* Ordinary untracked directories are fully enumerated by Git. */
  assert(!mkdir("ordinary",0700)); put("ordinary/data","ok",2);
  call(L,"review_snapshot_list",root,NULL,0);
  assert(has(L,"ordinary/data","file"));
  /* Ignored repositories remain excluded, without traversing their contents. */
  assert(!chdir("ignored")); git("init","-q"); assert(!chdir(root));
  call(L,"review_snapshot_list",root,NULL,0);
  assert(has(L,"ordinary/data","file") && !has(L,"ignored","directory"));
  assert(!has(L,"ignored/data","file"));
  /* An unregistered nested repository must not masquerade as a complete list. */
  assert(!mkdir("nested-repo",0700)); assert(!chdir("nested-repo"));
  git("init","-q"); put("data","nested",6); assert(!chdir(root));
  call(L,"review_snapshot_list",root,NULL,0);
  assert(lua_isnil(L,1));
  assert(!strcmp(lua_tostring(L,2),"snapshot enumeration incomplete"));
  /* Register the same directory as a gitlink: it is now a submodule boundary.
   * Index-only registration avoids network access or reading nested contents. */
  git("update-index","160000,1111111111111111111111111111111111111111,nested-repo");
  call(L,"review_snapshot_list",root,NULL,0);
  assert(has(L,"nested-repo","directory"));
  assert(!has(L,"nested-repo/data","file") && has(L,"ordinary/data","file"));
  /* Tracked paths replaced by links do not allow opening their descendants. */
  assert(!rmdir("dir")); assert(!symlink("ignored","dir"));
  call(L,"review_snapshot_list",root,NULL,0); assert(lua_istable(L,1));
  call(L,"review_snapshot_read",root,"dir/data",3); assert(lua_isnil(L,1));
  lua_close(L); assert(!chdir(cwd)); remove_tree(template);
  puts("review snapshot native: passed"); return 0;
}
