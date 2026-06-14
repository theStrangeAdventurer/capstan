CC = gcc
NCURSES_DIR = vendor/ncurses-install
LUA_DIR = vendor/lua-5.5.0
MUNIT_DIR = vendor/munit

CFLAGS = -Iinclude -I$(LUA_DIR)/src -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L
# -L - флаг для динамических библиотек, в нашем случае нужна статика
# LDFLAGS = -L$(NCURSES_DIR)/lib -lncursesw 
# А НАМ НУЖНО СТАТИЧЕСКИ
# Статическая библиотека - это файл с расширением .a (сокращенно от archive)
# Дополнительно добавляем libtinfow.a - когда собираем с --with-termlib то всякие константы типа "_COLS" уходят туда 
# -lm математическая библиотека - lua на нее ссылается
LDFLAGS = $(LUA_DIR)/src/liblua.a $(NCURSES_DIR)/lib/libncursesw.a  $(NCURSES_DIR)/lib/libtinfow.a -lm  -lcurl

TEST_CFLAGS = -Iinclude -I$(MUNIT_DIR) -std=gnu99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L
TEST_SRCS = src/input.c src/scroll.c src/utils.c test/test_main.c test/test_input.c test/test_scroll.c test/test_utils.c vendor/munit/munit.c

SRCS = $(wildcard src/*.c)

TARGET = build/termai
TEST_TARGET = build/test_runner

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET) 

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS)
	mkdir -p build
	$(CC) $(TEST_CFLAGS) $(TEST_SRCS) -o $(TEST_TARGET)

clean:
	rm -rf build
