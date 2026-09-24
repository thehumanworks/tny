CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
all: app
app: main.c mathutil.c mathutil.h
	$(CC) $(CFLAGS) -Iinclude main.c mathutil.c -lmissing_math -o app
test: app
	./app
