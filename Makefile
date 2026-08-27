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
TEST_SRCS = src/app_config.c src/cli_args.c src/clipboard.c src/diff_highlight.c src/dispatch_logic.c src/finder.c src/input.c src/input_history.c src/linemap.c src/mode.c src/permit_logic.c src/permit_prompt.c src/popup_logic.c src/project_instructions.c src/redact.c src/scroll.c src/session.c src/shell_process.c src/skills.c src/start_screen.c src/submission_queue.c src/tool_status.c src/tui_layout.c src/usage.c src/utils.c src/visual.c src/wiki.c test/test_main.c test/test_app_config.c test/test_cli_args.c test/test_clipboard.c test/test_diff_highlight.c test/test_dispatch.c test/test_finder.c test/test_input.c test/test_input_history.c test/test_linemap.c test/test_mode.c test/test_permit_logic.c test/test_permit_prompt.c test/test_popup.c test/test_project_instructions.c test/test_redact.c test/test_scroll.c test/test_session.c test/test_shell_process.c test/test_start_screen.c test/test_skills.c test/test_submission_queue.c test/test_tool_status.c test/test_tui_layout.c test/test_usage.c test/test_utils.c test/test_visual.c test/test_wiki.c vendor/munit/munit.c

CORE_PLUGIN_ASSETS = plugins/file.lua plugins/file_write.lua plugins/file_edit.lua plugins/shell.lua plugins/fetch.lua plugins/logs.lua plugins/skills.lua plugins/wiki.lua plugins/models.lua plugins/info.lua plugins/mcp.lua plugins/plan.lua plugins/implement.lua plugins/auth.lua plugins/logout.lua plugins/connect.lua plugins/vcs.lua
PROFILE_ASSETS = profiles/implement.lua profiles/plan.lua
AGENT_RUNTIME_ASSETS = agent/runtime.lua agent/acp.lua agent/provider_config.lua agent/models.lua agent/stream.lua agent/tools.lua agent/workspace.lua agent/tokens.lua agent/images.lua agent/logging.lua agent/ui.lua agent/utf8.lua agent/hooks.lua agent/state.lua agent/auth.lua agent/lua_serialize.lua agent/shell_safe.lua agent/mcp.lua agent/profiles.lua agent/redact.lua agent/vcs.lua $(PROFILE_ASSETS)
EMBEDDED_ASSETS = $(AGENT_RUNTIME_ASSETS) ai/system_prompt.txt vendor/rxi/json.lua skills/self-improvement/SKILL.md skills/wiki-onboarding/SKILL.md $(CORE_PLUGIN_ASSETS)
EMBEDDED_SRCS = build/embedded_assets.c
SRCS = $(wildcard src/*.c) $(EMBEDDED_SRCS)

TARGET = build/capstan
TEST_TARGET = build/test_runner

.PHONY: all clean test test-build test-acp test-http-lua test-openrouter-vision

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

test-openrouter-vision:
	sh test/test_openrouter_vision.sh

$(TEST_TARGET): $(TEST_SRCS)
	mkdir -p build
	$(CC) $(TEST_CFLAGS) $(TEST_SRCS) -o $(TEST_TARGET)

clean:
	rm -rf build

HTTP_LUA_FLAGS = -Iinclude -I$(LUA_DIR)/src -I$(MUNIT_DIR) -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE
HTTP_LUA_SRCS = src/agent.c src/app_config.c src/http.c src/log.c src/redact.c src/session.c src/session_manager.c src/utils.c test/test_agent.c test/test_http_stack.c test/test_fetch_plugin.c test/test_file_plugin.c test/test_file_write_plugin.c test/test_file_edit_plugin.c test/test_shell_plugin.c test/test_http_redirect.c test/test_logs_plugin.c test/test_log.c test/test_models_plugin.c test/test_info_plugin.c test/test_provider_tools.c test/test_skills_plugin.c test/test_wiki_plugin.c test/test_auth_lua.c test/test_main_http_stack.c vendor/munit/munit.c
HTTP_LUA_TARGET = build/test_http_stack

test-http-lua: $(HTTP_LUA_TARGET)
	./$(HTTP_LUA_TARGET)

$(HTTP_LUA_TARGET): $(HTTP_LUA_SRCS) $(AGENT_RUNTIME_ASSETS) $(CORE_PLUGIN_ASSETS)
	mkdir -p build
	$(CC) $(HTTP_LUA_FLAGS) $(HTTP_LUA_SRCS) $(LUA_DIR)/src/liblua.a -lm -lcurl -o $(HTTP_LUA_TARGET)
