#include "tokenizer.h"
#include <assert.h>
#include <string.h>
int main(void){size_t n=0;char **v=split_words("red blue",&n);assert(v&&n==2&&strcmp(v[0],"red")==0&&strcmp(v[1],"blue")==0&&v[2]==NULL);free_words(v);return 0;}
