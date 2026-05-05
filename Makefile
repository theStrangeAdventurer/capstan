CC = gcc
NCURSES_DIR = vendor/ncurses-install
LUA_DIR = vendor/lua-5.5.0

CFLAGS = -Iinclude -I$(LUA_DIR)/src -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200112L
# -L - флаг для динамических библиотек, в нашем случае нужна статика
# LDFLAGS = -L$(NCURSES_DIR)/lib -lncursesw 
# А НАМ НУЖНО СТАТИЧЕСКИ
# Статическая библиотека - это файл с расширением .a (сокращенно от archive)
# Дополнительно добавляем libtinfow.a - когда собираем с --with-termlib то всякие константы типа "_COLS" уходят туда 
# -lm математическая библиотека - lua на нее ссылается
LDFLAGS = $(LUA_DIR)/src/liblua.a $(NCURSES_DIR)/lib/libncursesw.a  $(NCURSES_DIR)/lib/libtinfow.a -lm  -lcurl

SRCS = $(wildcard src/*.c)

TARGET = build/termai

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET) 

clean:
	rm -rf build
