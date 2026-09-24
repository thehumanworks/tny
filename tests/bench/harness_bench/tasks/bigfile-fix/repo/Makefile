CC ?= cc
CFLAGS ?= -std=c11 -O0 -Wall -Wextra -Werror
all: test_scan
test_scan: aggregates.c test_scan.c aggregates.h
	$(CC) $(CFLAGS) aggregates.c test_scan.c -o $@
test: test_scan
	./test_scan
