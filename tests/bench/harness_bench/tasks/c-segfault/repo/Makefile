CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g -fsanitize=address
all: test_words
test_words: tokenizer.c tokenizer.h test.c
	$(CC) $(CFLAGS) tokenizer.c test.c -o $@
test: test_words
	./test_words
