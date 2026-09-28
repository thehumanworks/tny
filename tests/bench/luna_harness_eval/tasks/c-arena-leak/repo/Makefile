CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g
all: arena_test
arena_test: arena.c arena.h test.c
	$(CC) $(CFLAGS) arena.c test.c -o $@
test: arena_test
	./arena_test
