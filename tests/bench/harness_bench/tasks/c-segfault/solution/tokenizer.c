#include "tokenizer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
char **split_words(const char *text,size_t *count){
    size_t n=0;
    const char *p=text;
    while(*p){
        while(isspace((unsigned char)*p))++p;
        if(!*p)break;
        ++n;
        while(*p&&!isspace((unsigned char)*p))++p;
    }
    char **words=malloc((n+1)*sizeof(*words));
    if(!words)return NULL;
    p=text;
    for(size_t i=0;i<n;++i){
        while(isspace((unsigned char)*p))++p;
        const char *start=p;
        while(*p&&!isspace((unsigned char)*p))++p;
        size_t len=(size_t)(p-start);
        words[i]=malloc(len+1);
        if(!words[i]){words[i]=NULL;free_words(words);return NULL;}
        memcpy(words[i],start,len);words[i][len]='\0';
    }
    words[n]=NULL;
    *count=n;
    return words;
}
void free_words(char **words){
    if(!words)return;
    for(size_t i=0;words[i];++i)free(words[i]);
    free(words);
}
