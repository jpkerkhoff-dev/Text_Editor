CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -g
VALGRIND ?= /usr/bin/valgrind
GDB ?= /usr/bin/gdb
TARGET = app
SRC_DIR = src
OBJ_DIR = build

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean run valgrind gdb

all: $(TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

run: $(TARGET)
	./$(TARGET)

valgrind: $(TARGET)
	@if [ -z "$(VALGRIND)" ] || [ ! -x "$(VALGRIND)" ]; then \
		echo "Error: valgrind executable not found. Install it or set VALGRIND=/path/to/valgrind"; \
		exit 127; \
	fi
	$(VALGRIND) --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) 2>&1

gdb: $(TARGET)
	@if ! command -v $(GDB) >/dev/null 2>&1; then \
		echo "Error: gdb not found on PATH"; \
		exit 127; \
	fi
	$(GDB) -q -ex 'break main' -ex 'run' ./$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
