CC = gcc
VERSION ?= local
NCURSES_DIR = vendor/ncurses-install
LUA_DIR = vendor/lua-5.5.0
MUNIT_DIR = vendor/munit

CFLAGS = -Iinclude -I$(LUA_DIR)/src -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE -DPOPUP_NCURSES -DAPP_VERSION_VALUE=\"$(VERSION)\"
# Link vendored ncurses and Lua statically via direct archive paths.
# libtinfow.a is required because the ncurses build uses --with-termlib.
# libm is needed by Lua; libcurl remains the only system-linked runtime dep.
LDFLAGS = $(LUA_DIR)/src/liblua.a $(NCURSES_DIR)/lib/libncursesw.a  $(NCURSES_DIR)/lib/libtinfow.a -lm  -lcurl

TEST_CFLAGS = -Iinclude -I$(MUNIT_DIR) -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE
TEST_SRCS = src/otlp_wire.c test/test_otlp_wire.c src/app_config.c src/cli_args.c src/clipboard.c src/diff_highlight.c src/dispatch_logic.c src/editor_prompt.c src/finder.c src/input.c src/input_history.c src/jsonl.c src/linemap.c src/markdown.c src/text_layout.c vendor/md4c/md4c.c src/mode.c src/permit_logic.c src/permit_prompt.c src/popup_logic.c src/project_instructions.c src/redact.c src/scroll.c src/session.c src/shell_process.c src/process_manager.c src/background_work.c src/process_observe.c test/test_process_manager.c test/test_background_work.c test/test_process_observe.c src/shell_output.c src/skills.c src/start_screen.c src/submission_queue.c src/tool_status.c src/trace.c src/tui_layout.c src/usage.c src/utils.c src/visual.c src/wiki.c src/workspace_status.c test/test_workspace_status.c test/test_main.c test/test_app_config.c test/test_cli_args.c test/test_clipboard.c test/test_diff_highlight.c test/test_dispatch.c test/test_editor.c test/test_finder.c test/test_input.c test/test_input_history.c test/test_jsonl.c test/test_linemap.c test/test_markdown.c test/test_mode.c test/test_permit_logic.c test/test_permit_prompt.c test/test_popup.c test/test_project_instructions.c test/test_redact.c test/test_scroll.c test/test_session.c test/test_shell_process.c test/test_shell_output.c test/test_start_screen.c test/test_skills.c test/test_submission_queue.c test/test_tool_status.c test/test_trace.c test/test_tui_layout.c test/test_usage.c test/test_utils.c test/test_visual.c test/test_wiki.c vendor/munit/munit.c

CORE_PLUGIN_ASSETS = plugins/file.lua plugins/file_write.lua plugins/file_edit.lua plugins/shell.lua plugins/fetch.lua plugins/logs.lua plugins/skills.lua plugins/wiki.lua plugins/models.lua plugins/info.lua plugins/mcp.lua plugins/plan.lua plugins/implement.lua plugins/auth.lua plugins/logout.lua plugins/connect.lua plugins/vcs.lua plugins/tasks.lua plugins/issues.lua plugins/completion_review.lua plugins/show.lua plugins/processes.lua plugins/process_stop.lua
PROFILE_ASSETS = profiles/implement.lua profiles/plan.lua
AGENT_RUNTIME_ASSETS = agent/telemetry.lua agent/runtime.lua agent/tasks.lua agent/issues.lua agent/review_verdict.lua agent/review_config.lua agent/review_snapshot.lua agent/completion_review.lua agent/acp.lua agent/provider_config.lua agent/models.lua agent/stream.lua agent/subagents.lua agent/tools.lua agent/tool_output.lua agent/workspace.lua agent/tokens.lua agent/images.lua agent/logging.lua agent/ui.lua agent/utf8.lua agent/hooks.lua agent/state.lua agent/auth.lua agent/lua_serialize.lua agent/shell_safe.lua agent/mcp.lua agent/profiles.lua agent/redact.lua agent/vcs.lua $(PROFILE_ASSETS)
EMBEDDED_ASSETS = $(AGENT_RUNTIME_ASSETS) agent/vcs_git_stats.sh ai/system_prompt.txt vendor/rxi/json.lua skills/self-improvement/SKILL.md skills/wiki-onboarding/SKILL.md $(CORE_PLUGIN_ASSETS)
EMBEDDED_SRCS = build/embedded_assets.c
SRCS = $(wildcard src/*.c) vendor/md4c/md4c.c $(EMBEDDED_SRCS)

TARGET = build/capstan
TEST_TARGET = build/test_runner

.PHONY: all clean test test-build test-acp test-http-lua test-openrouter-vision test-tui-input

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET) 

$(EMBEDDED_SRCS): $(EMBEDDED_ASSETS) tools/embed_assets.sh
	sh tools/embed_assets.sh $(EMBEDDED_SRCS) $(EMBEDDED_ASSETS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

test-build: $(TARGET)
	sh test/test_acp.sh $(TARGET)
	sh test/test_build_smoke.sh $(TARGET)
	sh test/test_release_package.sh $(TARGET)
	sh test/test_installer.sh $(TARGET)

test-acp: $(TARGET)
	sh test/test_acp.sh $(TARGET)

test-tui-input: $(TARGET)
	python3 test/test_tui_wait_input.py $(TARGET)

test-openrouter-vision:
	sh test/test_openrouter_vision.sh

$(TEST_TARGET): $(TEST_SRCS)
	mkdir -p build
	$(CC) $(TEST_CFLAGS) $(TEST_SRCS) -o $(TEST_TARGET)

clean:
	rm -rf build

HTTP_LUA_FLAGS = -Iinclude -I$(LUA_DIR)/src -I$(MUNIT_DIR) -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE
HTTP_LUA_SRCS = src/otlp_wire.c src/telemetry.c src/agent.c src/shell_output.c src/app_config.c src/http.c src/jsonl.c src/log.c src/redact.c src/session.c src/session_manager.c src/utils.c test/test_agent.c test/test_http_stack.c test/test_fetch_plugin.c test/test_file_plugin.c test/test_file_write_plugin.c test/test_file_edit_plugin.c test/test_shell_plugin.c test/test_http_redirect.c test/test_logs_plugin.c test/test_log.c test/test_models_plugin.c test/test_info_plugin.c test/test_provider_tools.c test/test_skills_plugin.c test/test_wiki_plugin.c test/test_auth_lua.c test/test_main_http_stack.c vendor/munit/munit.c
HTTP_LUA_TARGET = build/test_http_stack

test-http-lua: $(HTTP_LUA_TARGET)
	./$(HTTP_LUA_TARGET)
	$(LUA_DIR)/src/lua test/test_telemetry.lua
	$(LUA_DIR)/src/lua test/test_runtime_timeline.lua
	$(LUA_DIR)/src/lua test/test_context_limits.lua
	$(LUA_DIR)/src/lua test/test_background_subagents.lua
	$(LUA_DIR)/src/lua test/test_internal_subagents.lua
	$(LUA_DIR)/src/lua test/test_issues.lua
	$(LUA_DIR)/src/lua test/test_review_config.lua
	$(LUA_DIR)/src/lua test/test_review_snapshot.lua
	$(LUA_DIR)/src/lua test/test_completion_review.lua
	$(LUA_DIR)/src/lua test/test_background_runtime.lua

.PHONY: test-telemetry
test-telemetry: build/test_telemetry_native
	python3 test/test_telemetry_otlp.py build/test_telemetry_native

build/test_telemetry_native: src/telemetry.c include/telemetry.h src/otlp_wire.c include/otlp_wire.h src/redact.c src/session.c src/utils.c src/app_config.c src/shell_output.c test/test_telemetry_native.c
	mkdir -p build
	$(CC) $(HTTP_LUA_FLAGS) src/telemetry.c src/otlp_wire.c src/redact.c src/session.c src/utils.c src/app_config.c src/shell_output.c test/test_telemetry_native.c $(LUA_DIR)/src/liblua.a -lm -lcurl -o $@

.PHONY: test-telemetry-modes
test-telemetry-modes: $(TARGET)
	python3 test/test_telemetry_modes.py $(TARGET)

.PHONY: test-telemetry-collector
test-telemetry-collector: build/test_telemetry_native
	python3 test/test_telemetry_collector.py $(COLLECTOR) build/test_telemetry_native

$(HTTP_LUA_TARGET): $(HTTP_LUA_SRCS) $(AGENT_RUNTIME_ASSETS) $(CORE_PLUGIN_ASSETS)
	mkdir -p build
	$(CC) $(HTTP_LUA_FLAGS) $(HTTP_LUA_SRCS) $(LUA_DIR)/src/liblua.a -lm -lcurl -o $(HTTP_LUA_TARGET)

# Process lifecycle, native adapters, model plugin contracts and live TUI.
.PHONY: test-process-control
test-process-control: $(TARGET) build/test_mcp_process build/test_process_panel build/test_process_bindings build/test_review_snapshot_native
	./build/test_review_snapshot_native
	./build/test_mcp_process
	./build/test_process_panel
	./build/test_process_bindings
	$(LUA_DIR)/src/lua test/test_mcp_reconcile.lua
	$(LUA_DIR)/src/lua test/test_process_tools.lua
	python3 test/test_process_tui.py $(TARGET)
	python3 test/test_background_modes.py $(TARGET)

.PHONY: test-completion-review-modes
test-completion-review-modes: $(TARGET)
	python3 test/test_completion_review_modes.py $(TARGET)

build/test_mcp_process: test/test_mcp_process.c src/mcp.c src/process_manager.c src/redact.c src/utils.c include/process_manager.h
	$(CC) $(HTTP_LUA_FLAGS) test/test_mcp_process.c src/mcp.c src/process_manager.c src/redact.c src/utils.c $(LUA_DIR)/src/liblua.a -lm -o $@

build/test_process_panel: test/test_process_panel.c src/process_panel.c src/process_observe.c include/process_panel.h include/process_observe.h
	$(CC) $(CFLAGS) src/process_panel.c src/process_observe.c test/test_process_panel.c $(NCURSES_DIR)/lib/libncursesw.a $(NCURSES_DIR)/lib/libtinfow.a -o $@

build/test_process_bindings: test/test_process_bindings.c src/permit.c src/review_snapshot.c src/permit_logic.c src/app_config.c src/utils.c src/process_manager.c src/background_work.c src/process_observe.c src/shell_process.c src/redact.c include/process_manager.h
	$(CC) $(HTTP_LUA_FLAGS) test/test_process_bindings.c src/permit.c src/review_snapshot.c src/permit_logic.c src/app_config.c src/utils.c src/process_manager.c src/background_work.c src/process_observe.c src/shell_process.c src/redact.c $(LUA_DIR)/src/liblua.a -lm -o $@

build/test_review_snapshot_native: src/review_snapshot.c include/review_snapshot.h test/test_review_snapshot_native.c
	mkdir -p build
	$(CC) $(HTTP_LUA_FLAGS) -DREVIEW_SNAPSHOT_TEST src/review_snapshot.c test/test_review_snapshot_native.c $(LUA_DIR)/src/liblua.a -lm -o $@
