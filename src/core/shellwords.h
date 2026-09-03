/* shellwords.h — POSIX-shell word splitting for the terminal-tool intercept
 * (docs/adr/0063).
 *
 * The splitter reproduces `/bin/sh` quoting (single quotes, double quotes,
 * backslash escapes) for a SINGLE SIMPLE COMMAND and stops at the first
 * unquoted character the shell would act on itself. It is a recogniser, not
 * an interpreter: whenever the answer is not obvious it refuses, and the
 * caller hands the command to the shell unchanged. */
#ifndef TNY_SHELLWORDS_H
#define TNY_SHELLWORDS_H

#include <stdbool.h>
#include <stddef.h>

#define TNY_WORDS_MAX 64

typedef struct {
    char **argv;  /* NULL-terminated, owned; NULL before a successful split */
    bool *quoted; /* argv[i] carried a quoted section */
    int argc;
    size_t consumed; /* bytes of input turned into argv */
    char stop;       /* 0 when the whole string was consumed, else the character */
} tny_words;

/* Split `input`. Returns 0 on success (`stop` says why it ended) and -1 when
 * the command cannot be classified safely: an unterminated quote, a trailing
 * backslash, an expansion inside double quotes, or more than TNY_WORDS_MAX
 * words. The struct is always safe to pass to tny_shellwords_free. */
int tny_shellwords(const char *input, tny_words *out);
void tny_shellwords_free(tny_words *w);

/* True for a character the shell would act on rather than pass through:
 * command separators, redirections, expansions, and pattern characters. */
bool tny_shellword_meta(char c);

#endif
