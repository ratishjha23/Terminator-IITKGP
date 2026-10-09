terminator: src/main.c src/shell.c include/shell.h
	gcc -Wall -Wextra -std=c11 -Iinclude src/main.c src/shell.c -o terminator