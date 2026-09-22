#include "permit.h"
#include "app_config.h"
#include <limits.h>
#include "process_manager.h"
#include "background_work.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

lua_State *L;
const char *session_manager_active_id(void) { return "manual-session"; }
int log_event(const char *category, const char *text) { (void)category; (void)text; return 1; }
const char *tui_permit_prompt(const char *tool, const char *target) {
  (void)tool; (void)target; return "deny";
}
void tui_pump_blocking(void) { process_manager_poll(); }
static void run(const char *script) {
  if (luaL_dostring(L, script) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    process_manager_shutdown(); exit(1);
  }
}
int main(void) {
  L = luaL_newstate(); assert(L); luaL_openlibs(L); tools_init(L);
  char original[PATH_MAX], cwd[PATH_MAX], child[PATH_MAX];
  assert(getcwd(original, sizeof(original)));
  assert(realpath("test", child));
  assert(app_workdir_set(original) && app_workspace_set(original));
  assert(chdir(child) == 0); /* Native workdir and actual cwd may differ. */
  lua_pushstring(L, child); lua_setglobal(L, "context_child");
  lua_pushstring(L, original); lua_setglobal(L, "context_root");
  run("saved_dir,saved_root,saved_cwd = tools.background_context(context_child,context_root); "
      "assert(saved_dir == context_root and saved_root == context_root and saved_cwd == context_child)");
  assert(!strcmp(app_workdir(), child));
  assert(!strcmp(app_workspace_root(), original));
  assert(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, child));
  run("assert(not pcall(tools.background_context,context_root,context_child)); "
      "assert(not pcall(tools.background_context,context_child,context_root,context_root..'/missing-cwd')); "
      "tools.background_context(saved_dir,saved_root,saved_cwd)");
  assert(!strcmp(app_workdir(), original));
  assert(!strcmp(app_workspace_root(), original));
  assert(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, child));
  assert(chdir(original) == 0);
  assert(app_execution_context_set(original, original, 0));
  run("saved_dir,saved_root,saved_cwd,saved_explicit = tools.background_context(context_child,context_root); "
      "assert(saved_explicit == false)");
  assert(app_workspace_explicit());
  run("tools.background_context(saved_dir,saved_root,saved_cwd,saved_explicit)");
  assert(!app_workspace_explicit());
  assert(!app_execution_context_set(original, child, 1));
  assert(!strcmp(app_workdir(), original) && !strcmp(app_workspace_root(), original));
  assert(!app_workspace_explicit());
  run("assert(not pcall(tools.background_context,context_root,context_child)); "
      "assert(not pcall(tools.background_context,context_child,context_root,context_root..'/missing-cwd'))");
  assert(!app_workspace_explicit());
  assert(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, original));
  run("tools.process_scope('session-a'); a = tools.shell('printf ready; sleep 30', 0, true); "
      "assert(a.started and a.id and a.pid and not a.exit); "
      "tools.process_scope('session-b'); assert(#tools.processes('list') == 0); "
      "assert(not pcall(tools.processes, 'get', a.id)); "
      "assert(not pcall(tools.process_stop, a.id)); "
      "assert(not pcall(tools.process_stop, tostring(a.pid))); "
      "b = tools.shell('sleep 30', 0, true); assert(tools.process_close_owner('session-a')); "
      "assert(tools.processes('get', b.id).running); tools.process_scope('session-a'); "
      "assert(tools.processes('get', a.id).stopping); "
      "assert(not tools.processes('get', a.id).running)");
  long long end = process_manager_now_ms() + 4000;
  ProcessSnapshot s;
  do {
    process_manager_poll();
    assert(process_manager_at(0, &s));
    assert(process_manager_now_ms() < end);
    struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
  } while (s.running);
  run("assert(#tools.process_events('session-b') == 0); "
      "local events = tools.process_events('session-a'); assert(#events == 1 and events[1].id == a.id); "
      "assert(#tools.process_events('session-a') == 0); "
      "tools.process_scope('session-b'); assert(tools.process_stop(b.id).stopping); "
      "tools.process_scope(nil); local c = tools.shell('true', 2); assert(c.exit == 0)");
  assert(process_manager_at(process_manager_count() - 1, &s));
  assert(!strcmp(s.owner, "manual-session"));
  run("tools.process_scope('session-a'); "
      "local id = tools.background_register{owner='session-a',kind='subagent',label='worker',workdir='.'}; "
      "local s = tools.processes('get',id); assert(s.kind == 'subagent' and s.status == 'queued'); "
      "assert(s.pid == nil and s.pgid == nil and s.descendants == nil); "
      "tools.process_scope('session-b'); assert(not pcall(tools.process_stop,id)); "
      "assert(not pcall(tools.processes,'output',id)); tools.process_scope('session-a'); "
      "assert(not tools.background_cancelled(id)); tools.process_stop(id); assert(tools.background_cancelled(id)); "
      "tools.background_update(id,{status='cancelled',output='done',ok=false}); "
      "assert(tools.processes('output',id).stdout == 'done'); "
      "assert(#tools.process_events('session-a') == 0); "
      "local group = tools.background_register{owner='session-a',kind='subagent_group'}; "
      "tools.background_update(group,{status='cancelled',output='{\"ok\":false}',ok=false}); "
      "local events = tools.process_events('session-a'); assert(#events == 1 and events[1].id == group); "
      "assert(events[1].status == 'cancelled' and events[1].ok == false); "
      "assert(#tools.process_events('session-a') == 0); "
      "assert(not pcall(tools.background_update,id,{status='completed',output='changed',ok=true})); "
      "assert(tools.processes('output',id).stdout == 'done'); "
      "local c = tools.background_register{owner='session-a',kind='subagent'}; "
      "agent_background_poll = function() if tools.background_cancelled(c) then "
      "tools.background_update(c,{status='cancelled',ok=false}) end end; "
      "local ok,n = tools.process_close_owner('session-a'); assert(ok and n == 1)");
  run("tools.process_scope('tasks'); "
      "local a = tools.background_register{kind='subagent'}; "
      "local b = tools.background_register{kind='subagent'}; "
      "local owner,task = tools.process_scope('tasks',a); assert(owner == 'tasks' and task == nil); "
      "local pa = tools.shell('sleep 30',0,true); assert(pa.task_id == a); "
      "owner,task = tools.process_scope('tasks',b); assert(owner == 'tasks' and task == a); "
      "local pb = tools.shell('sleep 30',0,true); assert(pb.task_id == b); "
      "owner,task = tools.process_scope('tasks'); assert(task == b); "
      "local job = tools.shell('sleep 30',0,true); assert(job.task_id == nil); "
      "tools.process_stop(a); assert(tools.processes('get',pa.id).stopping); "
      "assert(not tools.processes('get',pb.id).stopping); "
      "tools.background_update(b,{status='cancelled'}); assert(tools.background_cancelled(b)); "
      "assert(tools.processes('get',pb.id).stopping); "
      "tools.background_update(a,{status='cancelled'}); "
      "assert(not pcall(tools.process_stop,a)); "
      "assert(tools.processes('get',job.id).running and not tools.processes('get',job.id).stopping); "
      "assert(tools.process_close_owner('tasks'))");
  run("tools.process_scope('release-owner'); "
      "assert(not pcall(tools.background_register,{notify='false'})); "
      "local id = tools.background_register{kind='subagent_group',notify=false}; "
      "assert(not pcall(tools.background_release,id)); "
      "tools.process_scope('other-owner'); "
      "assert(not pcall(tools.background_update,id,{status='completed'})); "
      "assert(not pcall(tools.background_release,id)); "
      "tools.process_scope('release-owner'); "
      "assert(tools.processes('get',id).running); "
      "tools.background_update(id,{status='completed',output='visible',ok=true}); "
      "assert(tools.processes('output',id).stdout == 'visible'); "
      "assert(#tools.process_events('release-owner') == 0); "
      "tools.process_scope('other-owner'); assert(not pcall(tools.background_release,id)); "
      "tools.process_scope('release-owner'); assert(tools.background_release(id)); "
      "assert(not pcall(tools.background_release,id)); "
      "assert(not pcall(tools.processes,'get',id)); "
      "assert(not pcall(tools.background_update,id,{status='completed'})); "
      "assert(not pcall(tools.background_release,'unknown')); "
      "tools.process_scope('session-a'); assert(not pcall(tools.background_release,a.id)); "
      "assert(not tools.processes('get',a.id).running); "
      "tools.process_scope(nil); "
      "local manual = tools.background_register{owner='another',kind='subagent_group',notify=true}; "
      "tools.background_update(manual,{status='completed',ok=true}); "
      "local events = tools.process_events('another'); assert(#events == 1 and events[1].id == manual); "
      "assert(tools.background_release(manual))");
  assert(process_manager_get("unknown", &s) == 0);
  assert(process_manager_at(0, &s));
  assert(!background_work_release(s.id));
  assert(process_manager_get(s.id, &s));
  background_work_shutdown(); lua_close(L);
  puts("process bindings: ownership, opaque IDs, completion delivery, owner cleanup passed");
  return 0;
}
