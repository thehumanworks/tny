/* shlex.h — POSIX-shell-aware classifier for command lines
 * (docs/features/permissions.md, docs/adr/0059).
 *
 * This is NOT a shell: it never expands, never executes, and never claims a
 * command is safe. It answers one question for the permission engine — is
 * this string a *single simple command* whose first word we may reason
 * about, or does it carry shell machinery (chaining, redirection,
 * substitution, env prefixes) that makes argv0 meaningless? Anything it
 * cannot fully account for is reported as not simple, so the caller fails
 * closed and prompts. */
#ifndef TNY_SHLEX_H
#define TNY_SHLEX_H

#include <stdbool.h>

#define SHLEX_TOK_MAX 256

typedef struct {
    /* First word of the command with quoting removed ("" when absent). */
    char argv0[SHLEX_TOK_MAX];
    /* First non-option word after argv0 ("" when absent): the subcommand of
     * multi-verb programs (git status, npm run, cargo build). */
    char verb[SHLEX_TOK_MAX];
    bool meta;          /* unquoted ; & | ( ) < > ` $ or newline */
    bool env_prefix;    /* leading NAME=VALUE assignment (FOO=1 cmd) */
    bool unterminated;  /* unclosed quote or trailing backslash */
    bool truncated;     /* a word did not fit SHLEX_TOK_MAX */
    bool dangerous_opt; /* an option known to run or write things (-exec, --pre) */
} shlex_cmd;

/* Classify `cmd` (may be NULL). Never fails; the flags carry the verdict. */
void shlex_parse(const char *cmd, shlex_cmd *out);

/* One simple command with a usable argv0: no metacharacters, no env prefix,
 * nothing truncated or unterminated. */
bool shlex_is_simple(const shlex_cmd *c);

/* Program name to match rules against: the basename of argv0 when argv0 is a
 * bare word or an absolute path in a standard system bin directory, else
 * NULL (a relative or unusual path may be anything). Points into `c`. */
const char *shlex_program(const shlex_cmd *c);

#endif
