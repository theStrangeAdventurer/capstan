CC = gcc
NCURSES_DIR = vendor/ncurses-install
LUA_DIR = vendor/lua-5.5.0
MUNIT_DIR = vendor/munit

CFLAGS = -Iinclude -I$(LUA_DIR)/src -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L -DPOPUP_NCURSES
# -L - флаг для динамических библиотек, в нашем случае нужна статика
# LDFLAGS = -L$(NCURSES_DIR)/lib -lncursesw 
# А НАМ НУЖНО СТАТИЧЕСКИ
# Статическая библиотека - это файл с расширением .a (сокращенно от archive)
# Дополнительно добавляем libtinfow.a - когда собираем с --with-termlib то всякие константы типа "_COLS" уходят туда 
# -lm математическая библиотека - lua на нее ссылается
LDFLAGS = $(LUA_DIR)/src/liblua.a $(NCURSES_DIR)/lib/libncursesw.a  $(NCURSES_DIR)/lib/libtinfow.a -lm  -lcurl

TEST_CFLAGS = -Iinclude -I$(MUNIT_DIR) -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L
TEST_SRCS = src/input.c src/linemap.c src/mode.c src/permit_prompt.c src/popup_logic.c src/scroll.c src/usage.c src/utils.c src/visual.c test/test_main.c test/test_input.c test/test_linemap.c test/test_mode.c test/test_permit_prompt.c test/test_popup.c test/test_scroll.c test/test_usage.c test/test_utils.c test/test_visual.c vendor/munit/munit.c

CORE_PLUGIN_ASSETS = plugins/file.lua plugins/file_write.lua plugins/shell.lua plugins/fetch.lua plugins/logs.lua
EMBEDDED_ASSETS = ai/providers.lua ai/system_prompt.txt vendor/rxi/json.lua $(CORE_PLUGIN_ASSETS)
EMBEDDED_SRCS = build/embedded_assets.c
SRCS = $(wildcard src/*.c) $(EMBEDDED_SRCS)

TARGET = build/capstan
TEST_TARGET = build/test_runner

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET) 

$(EMBEDDED_SRCS): $(EMBEDDED_ASSETS) tools/embed_assets.sh
	sh tools/embed_assets.sh $(EMBEDDED_SRCS) $(EMBEDDED_ASSETS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

test-build: $(TARGET)
	sh test/test_build_smoke.sh $(TARGET)

$(TEST_TARGET): $(TEST_SRCS)
	mkdir -p build
	$(CC) $(TEST_CFLAGS) $(TEST_SRCS) -o $(TEST_TARGET)

clean:
	rm -rf build

HTTP_LUA_FLAGS = -Iinclude -I$(LUA_DIR)/src -I$(MUNIT_DIR) -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L
HTTP_LUA_SRCS = src/http.c test/test_http_stack.c test/test_fetch_plugin.c test/test_file_plugin.c test/test_file_write_plugin.c test/test_http_redirect.c test/test_logs_plugin.c test/test_provider_tools.c test/test_main_http_stack.c vendor/munit/munit.c
HTTP_LUA_TARGET = build/test_http_stack

test-http-lua: $(HTTP_LUA_TARGET)
	./$(HTTP_LUA_TARGET)

$(HTTP_LUA_TARGET): $(HTTP_LUA_SRCS)
	mkdir -p build
	$(CC) $(HTTP_LUA_FLAGS) $(HTTP_LUA_SRCS) $(LUA_DIR)/src/liblua.a -lm -lcurl -o $(HTTP_LUA_TARGET)
