CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
all: app
app: main.c mathutil.c mathutil.h
	$(CC) $(CFLAGS) -I. main.c mathutil.c -o app
test: app
	./app
