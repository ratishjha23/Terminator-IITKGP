
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude
GUI_LIBS = -lX11

# Existing CLI shell
terminator: src/main.c src/shell.c include/shell.h
	$(CC) $(CFLAGS) src/main.c src/shell.c -o terminator

# GUI terminal

terminal_gui: src/terminal_gui.c src/executor.c src/output_buffer.c src/parser.c src/history.c include/executor.h include/output_buffer.h include/parser.h include/history.h
	$(CC) $(CFLAGS) src/terminal_gui.c src/executor.c src/output_buffer.c src/parser.c src/history.c -o terminal_gui $(GUI_LIBS)

clean:
	rm -f terminator terminal_gui

.PHONY: clean
