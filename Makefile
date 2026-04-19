CC = gcc
NCURSES_DIR = vendor/ncurses-install

CFLAGS = -Iinclude -I$(NCURSES_DIR)/include -I$(NCURSES_DIR)/include/ncursesw
# -L - флаг для динамических библиотек, в нашем случае нужна статика
# LDFLAGS = -L$(NCURSES_DIR)/lib -lncursesw 
# А НАМ НУЖНО СТАТИЧЕСКИ
# Статическая библиотека - это файл с расширением .a (сокращенно от archive)
# Дополнительно добавляем libtinfow.a - когда собираем с --with-termlib то всякие константы типа "_COLS" уходят туда 
LDFLAGS = $(NCURSES_DIR)/lib/libncursesw.a  $(NCURSES_DIR)/lib/libtinfow.a

SRCS = $(wildcard src/*.c)

TARGET = build/termai

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET) 

clean:
	rm -rf build
