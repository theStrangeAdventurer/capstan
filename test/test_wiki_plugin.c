#include "munit.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int l_ctx_replace(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  return 2;
}

static int l_ctx_error(lua_State *L) {
  const char *ui_val = luaL_checkstring(L, 2);
  const char *llm_val = lua_isnoneornil(L, 3) ? ui_val : luaL_checkstring(L, 3);
  lua_pushstring(L, ui_val);
  lua_pushstring(L, llm_val);
  lua_pushboolean(L, 0);
  return 3;
}

static int l_capstan_realpath(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  char resolved[4096];
  if (!realpath(path, resolved)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, resolved);
  return 1;
}

static lua_State *new_state(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  return L;
}

static void set_unconfigured_capstan(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);
  lua_setfield(L, -2, "config");
  lua_setglobal(L, "capstan");
}

static void set_configured_capstan(lua_State *L, const char *wiki_path) {
  lua_newtable(L);
  lua_pushcfunction(L, l_capstan_realpath);
  lua_setfield(L, -2, "realpath");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, wiki_path);
  lua_setfield(L, -2, "path");
  lua_setfield(L, -2, "wiki");
  lua_setfield(L, -2, "config");
  lua_pushstring(L, "configured wiki summary");
  lua_setfield(L, -2, "wiki_summary");
  lua_setglobal(L, "capstan");
}

static void load_wiki_plugin(lua_State *L) {
  int rc = luaL_dofile(L, "plugins/wiki.lua");
  munit_assert_int(rc, ==, LUA_OK);
  munit_assert_true(lua_istable(L, -1));
}

static void call_handler(lua_State *L, const char *arg1, const char *arg2) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_newtable(L);
  if (arg1) {
    lua_pushstring(L, arg1);
    lua_rawseti(L, -2, 1);
  }
  if (arg2) {
    lua_pushstring(L, arg2);
    lua_rawseti(L, -2, 2);
  }
  lua_setfield(L, -2, "args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  lua_pushcfunction(L, l_ctx_error);
  lua_setfield(L, -2, "error");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_tool_handler(lua_State *L, const char *tool_name,
                              const char *source, const char *path) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_pushstring(L, tool_name);
  lua_setfield(L, -2, "tool_name");
  lua_newtable(L);
  lua_setfield(L, -2, "args");
  lua_newtable(L);
  if (source) {
    lua_pushstring(L, source);
    lua_setfield(L, -2, "source");
  }
  if (path) {
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
  }
  lua_setfield(L, -2, "tool_args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  lua_pushcfunction(L, l_ctx_error);
  lua_setfield(L, -2, "error");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void call_wiki_ingest_tool_handler(lua_State *L, const char *path,
                                          int copy,
                                          const char *target_dir) {
  lua_getfield(L, -1, "handler");

  lua_newtable(L);
  lua_pushstring(L, "wiki_ingest");
  lua_setfield(L, -2, "tool_name");
  lua_newtable(L);
  lua_setfield(L, -2, "args");
  lua_newtable(L);
  if (path) {
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
  }
  if (copy) {
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "copy");
  }
  if (target_dir) {
    lua_pushstring(L, target_dir);
    lua_setfield(L, -2, "target_dir");
  }
  lua_setfield(L, -2, "tool_args");
  lua_pushcfunction(L, l_ctx_replace);
  lua_setfield(L, -2, "replace");
  lua_pushcfunction(L, l_ctx_error);
  lua_setfield(L, -2, "error");

  int rc = lua_pcall(L, 1, 2, 0);
  munit_assert_int(rc, ==, LUA_OK);
}

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  munit_assert_not_null(f);
  fputs(content, f);
  fclose(f);
}

static char *read_file_alloc(const char *path) {
  FILE *f = fopen(path, "rb");
  munit_assert_not_null(f);
  munit_assert_int(fseek(f, 0, SEEK_END), ==, 0);
  long size = ftell(f);
  munit_assert_true(size >= 0);
  rewind(f);
  char *content = malloc((size_t)size + 1);
  munit_assert_not_null(content);
  munit_assert_size(fread(content, 1, (size_t)size, f), ==, (size_t)size);
  content[size] = '\0';
  fclose(f);
  return content;
}

static void install_ingest_agent_stub(lua_State *L) {
  const char *chunk =
      "capstan.models = { weak = function() return { provider = 'weak-provider', model = 'weak-model' } end }\n"
      "capstan.agent = { run = function(opts, callbacks)\n"
      "  _G.last_provider = opts.provider\n"
      "  _G.last_model = opts.model\n"
      "  _G.last_tools_count = #(opts.tools or {})\n"
      "  _G.last_ingest_prompt = opts.messages[1].content\n"
      "  if callbacks and callbacks.on_text then callbacks.on_text('{\"entries\":[{\"path\":\"guide.md\",\"id\":\"guide\",\"kind\":\"source\",\"title\":\"Guide\",\"description\":\"Indexed guide.\",\"use_when\":[\"When guide details matter.\"],\"tags\":[\"guide\"],\"index_policy\":\"always\",\"context_policy\":\"retrieve_only\"}]}') end\n"
      "  if callbacks and callbacks.on_done then callbacks.on_done({ ok = true, text = '' }) end\n"
      "  return true\n"
      "end }\n"
      "popup = { info = function(title, message) _G.last_popup = tostring(title) .. ':' .. tostring(message) end, error = function(title, message) _G.last_error = tostring(title) .. ':' .. tostring(message) end }\n";
  int rc = luaL_dostring(L, chunk);
  munit_assert_int(rc, ==, LUA_OK);
}

static void install_ingest_agent_error_stub(lua_State *L) {
  const char *chunk =
      "capstan.models = { weak = function() return { provider = 'weak-provider', model = 'weak-model' } end }\n"
      "capstan.agent = { run = function(opts, callbacks)\n"
      "  if callbacks and callbacks.on_error then callbacks.on_error('HTTP 400 bad unicode') end\n"
      "  if callbacks and callbacks.on_done then callbacks.on_done({ ok = false, error = 'HTTP 400 bad unicode', text = '' }) end\n"
      "  return true\n"
      "end }\n"
      "popup = { info = function(title, message) _G.last_popup = tostring(title) .. ':' .. tostring(message) end, error = function(title, message) _G.last_error = tostring(title) .. ':' .. tostring(message) end }\n";
  int rc = luaL_dostring(L, chunk);
  munit_assert_int(rc, ==, LUA_OK);
}

static void install_ingest_agent_wrapped_payload_stub(lua_State *L) {
  const char *chunk =
      "capstan.models = { weak = function() return { provider = 'weak-provider', model = 'weak-model' } end }\n"
      "capstan.agent = { run = function(opts, callbacks)\n"
      "  if callbacks and callbacks.on_text then callbacks.on_text('```python\\ndata = {\"source_id\":\"x\",\"files\":[{\"path\":\"guide.md\"}]}\\n```') end\n"
      "  if callbacks and callbacks.on_done then callbacks.on_done({ ok = true, text = '' }) end\n"
      "  return true\n"
      "end }\n"
      "popup = { info = function(title, message) _G.last_popup = tostring(title) .. ':' .. tostring(message) end, error = function(title, message) _G.last_error = tostring(title) .. ':' .. tostring(message) end }\n";
  int rc = luaL_dostring(L, chunk);
  munit_assert_int(rc, ==, LUA_OK);
}

static int first_json_id(const char *dir, char *out, size_t out_size) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len > 5 && strcmp(entry->d_name + len - 5, ".json") == 0) {
      if (len - 5 >= out_size) {
        closedir(d);
        return 0;
      }
      memcpy(out, entry->d_name, len - 5);
      out[len - 5] = '\0';
      closedir(d);
      return 1;
    }
  }
  closedir(d);
  return 0;
}

static MunitResult test_unconfigured_wiki_starts_onboarding(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  set_unconfigured_capstan(L);
  load_wiki_plugin(L);
  call_handler(L, NULL, NULL);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, "Capstan Wiki is not initialized yet") != NULL);
  munit_assert_true(strstr(ui, "wiki-onboarding") != NULL);
  munit_assert_string_equal(ui, llm);

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_wiki_read_tool_is_internal_permission_free(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  set_unconfigured_capstan(L);
  load_wiki_plugin(L);
  lua_getfield(L, -1, "tools");
  munit_assert_true(lua_istable(L, -1));
  lua_rawgeti(L, -1, 1);
  lua_getfield(L, -1, "name");
  munit_assert_string_equal(lua_tostring(L, -1), "wiki_read");
  lua_pop(L, 1);
  lua_getfield(L, -1, "permission");
  munit_assert_true(lua_isboolean(L, -1));
  munit_assert_false(lua_toboolean(L, -1));

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_configured_status_returns_summary(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  lua_State *L = new_state();
  set_configured_capstan(L, "/tmp/capstan-wiki-configured");
  load_wiki_plugin(L);
  call_handler(L, "status", NULL);

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_string_equal(ui, "configured wiki summary");

  lua_close(L);
  return MUNIT_OK;
}

static MunitResult test_read_rejects_parent_traversal(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  load_wiki_plugin(L);
  call_handler(L, "read", "../outside.md");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "escapes configured wiki directory") != NULL);

  lua_close(L);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_read_reads_relative_file(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-read-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char note[4096];
  snprintf(note, sizeof(note), "%s/note.md", root);
  write_file(note, "wiki note body");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  load_wiki_plugin(L);
  call_handler(L, "read", "note.md");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Wiki file: note.md") != NULL);
  munit_assert_true(strstr(ui, "wiki note body") != NULL);

  lua_close(L);
  unlink(note);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_ingest_uses_weak_model_and_writes_source_index(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-ingest-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);
  char guide[4096];
  snprintf(guide, sizeof(guide), "%s/guide.md", source);
  write_file(guide, "# Guide\n\nsource body");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_stub(L);
  load_wiki_plugin(L);
  call_handler(L, "ingest", source);

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Wiki ingest completed") != NULL);
  lua_getglobal(L, "last_provider");
  munit_assert_string_equal(lua_tostring(L, -1), "weak-provider");
  lua_pop(L, 1);
  lua_getglobal(L, "last_model");
  munit_assert_string_equal(lua_tostring(L, -1), "weak-model");
  lua_pop(L, 1);
  lua_getglobal(L, "last_tools_count");
  munit_assert_int((int)lua_tointeger(L, -1), ==, 0);
  lua_pop(L, 3);

  char index_dir[4096];
  snprintf(index_dir, sizeof(index_dir), "%s/index", root);
  char source_id[256];
  munit_assert_true(first_json_id(index_dir, source_id, sizeof(source_id)));
  char index_file[4096];
  snprintf(index_file, sizeof(index_file), "%s/%s.json", index_dir, source_id);
  char *content = NULL;
  FILE *f = fopen(index_file, "rb");
  munit_assert_not_null(f);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);
  content = malloc((size_t)size + 1);
  munit_assert_not_null(content);
  munit_assert_size(fread(content, 1, (size_t)size, f), ==, (size_t)size);
  content[size] = '\0';
  fclose(f);
  munit_assert_true(strstr(content, "\"source_root\"") != NULL);
  munit_assert_true(strstr(content, "\"path\":\"guide.md\"") != NULL);
  munit_assert_true(strstr(content, "\"description\":\"Indexed guide.\"") != NULL);
  free(content);

  call_tool_handler(L, "wiki_source_read", source_id, "guide.md");
  const char *tool_ui = lua_tostring(L, -2);
  munit_assert_not_null(tool_ui);
  munit_assert_true(strstr(tool_ui, "Wiki source file:") != NULL);
  munit_assert_true(strstr(tool_ui, "source body") != NULL);
  lua_pop(L, 2);

  call_tool_handler(L, "wiki_source_read", source_id, "missing.md");
  const char *denied = lua_tostring(L, -2);
  munit_assert_not_null(denied);
  munit_assert_true(strstr(denied, "path is not present in source index") != NULL);

  lua_close(L);
  unlink(index_file);
  rmdir(index_dir);
  unlink(guide);
  rmdir(source);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_tool_writes_source_index(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-tool-ingest-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-tool-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);
  char guide[4096];
  snprintf(guide, sizeof(guide), "%s/guide.md", source);
  write_file(guide, "# Guide\n\nsource body");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_stub(L);
  load_wiki_plugin(L);
  call_wiki_ingest_tool_handler(L, source, 0, NULL);

  const char *ui = lua_tostring(L, -2);
  const char *llm = lua_tostring(L, -1);
  munit_assert_not_null(ui);
  munit_assert_not_null(llm);
  munit_assert_true(strstr(ui, "Wiki ingest completed") != NULL);
  munit_assert_string_equal(ui, llm);

  char index_dir[4096];
  snprintf(index_dir, sizeof(index_dir), "%s/index", root);
  char source_id[256];
  munit_assert_true(first_json_id(index_dir, source_id, sizeof(source_id)));
  char index_file[4096];
  snprintf(index_file, sizeof(index_file), "%s/%s.json", index_dir, source_id);
  char *content = read_file_alloc(index_file);
  munit_assert_true(strstr(content, "\"mode\":\"external\"") != NULL);
  munit_assert_true(strstr(content, "\"path\":\"guide.md\"") != NULL);
  free(content);

  lua_close(L);
  unlink(index_file);
  rmdir(index_dir);
  unlink(guide);
  rmdir(source);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_tool_copy_validates_target_dir(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-tool-copy-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-tool-copy-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);
  char guide[4096];
  snprintf(guide, sizeof(guide), "%s/guide.md", source);
  write_file(guide, "# Guide\n\nsource body");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_stub(L);
  load_wiki_plugin(L);
  call_wiki_ingest_tool_handler(L, source, 1, "sources/imported");

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Wiki ingest completed") != NULL);
  lua_pop(L, 2);

  char copied[4096];
  snprintf(copied, sizeof(copied), "%s/sources/imported/guide.md", root);
  char *copied_content = read_file_alloc(copied);
  munit_assert_true(strstr(copied_content, "schema_version: 1") != NULL);
  munit_assert_true(strstr(copied_content, "# Guide") != NULL);
  free(copied_content);

  call_wiki_ingest_tool_handler(L, source, 1, "../outside");
  const char *denied = lua_tostring(L, -2);
  munit_assert_not_null(denied);
  munit_assert_true(strstr(denied, "copy target must be a relative wiki directory") != NULL);

  char index_dir[4096];
  snprintf(index_dir, sizeof(index_dir), "%s/index", root);
  char source_id[256];
  munit_assert_true(first_json_id(index_dir, source_id, sizeof(source_id)));
  char index_file[4096];
  snprintf(index_file, sizeof(index_file), "%s/%s.json", index_dir, source_id);

  lua_close(L);
  unlink(index_file);
  rmdir(index_dir);
  unlink(copied);
  char imported_dir[4096];
  snprintf(imported_dir, sizeof(imported_dir), "%s/sources/imported", root);
  rmdir(imported_dir);
  char sources_dir[4096];
  snprintf(sources_dir, sizeof(sources_dir), "%s/sources", root);
  rmdir(sources_dir);
  rmdir(root);
  unlink(guide);
  rmdir(source);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_tool_falls_back_on_weak_model_error(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-tool-error-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-tool-error-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);
  char guide[4096];
  snprintf(guide, sizeof(guide), "%s/guide.md", source);
  write_file(guide, "# Guide\n\nsource body");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_error_stub(L);
  load_wiki_plugin(L);
  call_wiki_ingest_tool_handler(L, source, 0, NULL);

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Wiki ingest completed with fallback metadata") != NULL);
  lua_getglobal(L, "last_popup");
  munit_assert_true(strstr(lua_tostring(L, -1), "fallback metadata") != NULL);

  char index_dir[4096];
  snprintf(index_dir, sizeof(index_dir), "%s/index", root);
  char source_id[256];
  munit_assert_true(first_json_id(index_dir, source_id, sizeof(source_id)));
  char index_file[4096];
  snprintf(index_file, sizeof(index_file), "%s/%s.json", index_dir, source_id);
  char *content = read_file_alloc(index_file);
  munit_assert_true(strstr(content, "\"mode\":\"external\"") != NULL);
  munit_assert_true(strstr(content, "\"source_root\"") != NULL);
  munit_assert_true(strstr(content, "\"path\":\"guide.md\"") != NULL);
  free(content);

  lua_close(L);
  unlink(index_file);
  rmdir(index_dir);
  unlink(guide);
  rmdir(source);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_falls_back_on_wrapped_non_entries_json(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-tool-wrapped-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-tool-wrapped-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);
  char guide[4096];
  snprintf(guide, sizeof(guide), "%s/guide.md", source);
  write_file(guide, "# Guide\n\nsource body");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_wrapped_payload_stub(L);
  load_wiki_plugin(L);
  call_wiki_ingest_tool_handler(L, source, 0, NULL);

  const char *ui = lua_tostring(L, -2);
  munit_assert_not_null(ui);
  munit_assert_true(strstr(ui, "Wiki ingest completed") != NULL);

  char index_dir[4096];
  snprintf(index_dir, sizeof(index_dir), "%s/index", root);
  char source_id[256];
  munit_assert_true(first_json_id(index_dir, source_id, sizeof(source_id)));
  char index_file[4096];
  snprintf(index_file, sizeof(index_file), "%s/%s.json", index_dir, source_id);
  char *content = read_file_alloc(index_file);
  munit_assert_true(strstr(content, "\"mode\":\"external\"") != NULL);
  munit_assert_true(strstr(content, "\"path\":\"guide.md\"") != NULL);
  free(content);

  lua_close(L);
  unlink(index_file);
  rmdir(index_dir);
  unlink(guide);
  rmdir(source);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_sanitizes_invalid_utf8(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-tool-utf8-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-tool-utf8-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);
  char guide[4096];
  snprintf(guide, sizeof(guide), "%s/guide.md", source);
  write_file(guide, "# Guide\n\nbad byte: \xff\nvalid utf8: \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n");

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_stub(L);
  load_wiki_plugin(L);
  call_wiki_ingest_tool_handler(L, source, 0, NULL);

  lua_getglobal(L, "last_ingest_prompt");
  size_t len = 0;
  const char *prompt = lua_tolstring(L, -1, &len);
  munit_assert_not_null(prompt);
  munit_assert_null(memchr(prompt, 0xff, len));
  munit_assert_true(strstr(prompt, "bad byte: ?") != NULL);
  munit_assert_true(strstr(prompt, "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82") != NULL);

  lua_close(L);
  unlink(guide);
  rmdir(source);
  rmdir(root);
  return MUNIT_OK;
}

static MunitResult test_wiki_ingest_caps_prompt_content_budget(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;

  char root[4096];
  snprintf(root, sizeof(root), "/tmp/capstan-wiki-plugin-tool-budget-%ld", (long)getpid());
  rmdir(root);
  munit_assert_int(mkdir(root, 0700), ==, 0);
  char source[4096];
  snprintf(source, sizeof(source), "/tmp/capstan-wiki-plugin-tool-budget-source-%ld", (long)getpid());
  rmdir(source);
  munit_assert_int(mkdir(source, 0700), ==, 0);

  char body[2048];
  memset(body, 'a', sizeof(body) - 1);
  body[sizeof(body) - 1] = '\0';
  for (int i = 0; i < 80; i++) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/file-%02d.md", source, i);
    write_file(path, body);
  }

  lua_State *L = new_state();
  set_configured_capstan(L, root);
  install_ingest_agent_stub(L);
  load_wiki_plugin(L);
  call_wiki_ingest_tool_handler(L, source, 0, NULL);

  lua_getglobal(L, "last_ingest_prompt");
  size_t len = 0;
  const char *prompt = lua_tolstring(L, -1, &len);
  munit_assert_not_null(prompt);
  munit_assert_size(len, <, 90000);

  lua_close(L);
  for (int i = 0; i < 80; i++) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/file-%02d.md", source, i);
    unlink(path);
  }
  rmdir(source);
  rmdir(root);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/unconfigured_wiki_starts_onboarding", test_unconfigured_wiki_starts_onboarding, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_read_tool_is_internal_permission_free",
     test_wiki_read_tool_is_internal_permission_free, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/configured_status_returns_summary", test_configured_status_returns_summary, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/read_rejects_parent_traversal", test_read_rejects_parent_traversal, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/read_reads_relative_file", test_read_reads_relative_file, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/ingest_uses_weak_model_and_writes_source_index",
     test_ingest_uses_weak_model_and_writes_source_index, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_tool_writes_source_index",
     test_wiki_ingest_tool_writes_source_index, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_tool_copy_validates_target_dir",
     test_wiki_ingest_tool_copy_validates_target_dir, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_tool_falls_back_on_weak_model_error",
     test_wiki_ingest_tool_falls_back_on_weak_model_error, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_falls_back_on_wrapped_non_entries_json",
     test_wiki_ingest_falls_back_on_wrapped_non_entries_json, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_sanitizes_invalid_utf8",
     test_wiki_ingest_sanitizes_invalid_utf8, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/wiki_ingest_caps_prompt_content_budget",
     test_wiki_ingest_caps_prompt_content_budget, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite wiki_plugin_suite = {"/wiki_plugin", tests, NULL, 1,
                               MUNIT_SUITE_OPTION_NONE};
