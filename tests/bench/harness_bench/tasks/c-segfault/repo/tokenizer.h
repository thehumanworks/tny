#ifndef TOKENIZER_H
#define TOKENIZER_H
#include <stddef.h>
char **split_words(const char *text,size_t *count);
void free_words(char **words);
#endif
