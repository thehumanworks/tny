#include "core/shlex.h"

#include <string.h>

/* Unquoted characters that turn one command line into several, redirect it,
 * or splice in the output of another command. Their presence means argv0 no
 * longer describes what will run. */
static bool meta_char(char ch, bool in_word) {
    if (ch == ';' || ch == '&' || ch == '|' || ch == '(' || ch == ')' || ch == '<' || ch == '>' ||
        ch == '`' || ch == '$' || ch == '\n' || ch == '\r')
        return true;
    /* `#` starts a comment and `{` a group command only at a word boundary;
     * inside a word they are ordinary characters (git log --format={x}). */
    return !in_word && (ch == '#' || ch == '{');
}

/* NAME=VALUE, the POSIX assignment prefix: `FOO=1 rm -rf /` runs rm. */
static bool is_assignment(const char *w) {
    if (!(*w == '_' || (*w >= 'A' && *w <= 'Z') || (*w >= 'a' && *w <= 'z'))) return false;
    for (const char *q = w + 1; *q; q++) {
        if (*q == '=') return true;
        bool ok = *q == '_' || (*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z') ||
                  (*q >= 'a' && *q <= 'z');
        if (!ok) return false;
    }
    return false;
}

/* Options that make an otherwise read-only program run or write things
 * (find -exec/-delete/-fprintf, ripgrep --pre). Option injection is how
 * "safe program" lists get bypassed, so the check is on the option name
 * only, `--opt=value` included. */
static bool dangerous_option(const char *tok) {
    if (tok[0] != '-') return false;
    char name[SHLEX_TOK_MAX];
    size_t n = 0;
    for (const char *q = tok; *q && *q != '=' && n + 1 < sizeof name; q++) name[n++] = *q;
    name[n] = '\0';
    if (strstr(name, "exec")) return true;
    static const char *bad[] = {"-delete", "-ok",      "-okdir", "-fprintf",   "-fls",
                                "-fprint", "-fprint0", "--pre",  "--pre-glob", "--hostname-bin",
                                NULL};
    for (int i = 0; bad[i]; i++)
        if (strcmp(name, bad[i]) == 0) return true;
    return false;
}

static void take_word(shlex_cmd *out, const char *tok, int index, bool over) {
    if (over) {
        out->truncated = true;
        return;
    }
    if (index == 0) {
        if (is_assignment(tok)) {
            out->env_prefix = true;
            return;
        }
        size_t n = strlen(tok);
        memcpy(out->argv0, tok, n + 1);
        return;
    }
    if (dangerous_option(tok)) out->dangerous_opt = true;
    if (!out->verb[0] && tok[0] != '-') {
        size_t n = strlen(tok);
        memcpy(out->verb, tok, n + 1);
    }
}

void shlex_parse(const char *cmd, shlex_cmd *out) {
    memset(out, 0, sizeof *out);
    if (!cmd) return;

    char tok[SHLEX_TOK_MAX];
    size_t tl = 0;
    bool in_tok = false, over = false;
    int index = 0;
    size_t i = 0;

    for (;;) {
        char ch = cmd[i];
        if (ch == '\0' || ch == ' ' || ch == '\t') {
            if (in_tok) {
                tok[tl] = '\0';
                take_word(out, tok, index++, over);
                tl = 0;
                in_tok = false;
                over = false;
                if (out->env_prefix) return; /* argv0 is not the program */
            }
            if (ch == '\0') break;
            i++;
            continue;
        }
        if (meta_char(ch, in_tok)) {
            out->meta = true;
            break;
        }
        if (ch == '\'') {
            in_tok = true;
            i++;
            bool closed = false;
            for (; cmd[i]; i++) {
                if (cmd[i] == '\'') {
                    closed = true;
                    i++;
                    break;
                }
                if (tl + 1 < sizeof tok) tok[tl++] = cmd[i];
                else over = true;
            }
            if (!closed) {
                out->unterminated = true;
                break;
            }
            continue;
        }
        if (ch == '"') {
            in_tok = true;
            i++;
            bool closed = false;
            for (; cmd[i]; i++) {
                char c = cmd[i];
                if (c == '"') {
                    closed = true;
                    i++;
                    break;
                }
                if (c == '$' || c == '`') { /* expansion survives double quotes */
                    out->meta = true;
                    return;
                }
                if (c == '\\' && cmd[i + 1]) {
                    char nx = cmd[i + 1];
                    if (nx == '"' || nx == '\\' || nx == '$' || nx == '`') {
                        c = nx;
                        i++;
                    }
                }
                if (tl + 1 < sizeof tok) tok[tl++] = c;
                else over = true;
            }
            if (!closed) {
                out->unterminated = true;
                break;
            }
            continue;
        }
        if (ch == '\\') {
            if (!cmd[i + 1]) {
                out->unterminated = true;
                break;
            }
            in_tok = true;
            if (cmd[i + 1] == '\n') {
                out->meta = true; /* line continuation: more command follows */
                break;
            }
            if (tl + 1 < sizeof tok) tok[tl++] = cmd[i + 1];
            else over = true;
            i += 2;
            continue;
        }
        in_tok = true;
        if (tl + 1 < sizeof tok) tok[tl++] = ch;
        else over = true;
        i++;
    }
}

bool shlex_is_simple(const shlex_cmd *c) {
    if (!c) return false;
    if (c->meta || c->env_prefix || c->unterminated || c->truncated) return false;
    return c->argv0[0] != '\0';
}

const char *shlex_program(const shlex_cmd *c) {
    if (!c || !c->argv0[0]) return NULL;
    const char *slash = strrchr(c->argv0, '/');
    if (!slash) return c->argv0;
    static const char *bin_dirs[] = {"/bin/",  "/usr/bin/",  "/usr/local/bin/",
                                     "/sbin/", "/usr/sbin/", "/opt/homebrew/bin/",
                                     NULL};
    for (int i = 0; bin_dirs[i]; i++) {
        size_t n = strlen(bin_dirs[i]);
        if (strncmp(c->argv0, bin_dirs[i], n) == 0 && (size_t)(slash - c->argv0) + 1 == n)
            return slash + 1;
    }
    return NULL; /* ./ls, ../tools/git, /tmp/x/git: not the system program */
}
