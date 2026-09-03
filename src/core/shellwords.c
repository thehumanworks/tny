/* shellwords.c — see shellwords.h. Recognise-or-refuse word splitting. */
#include "core/shellwords.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

bool tny_shellword_meta(char c) { return strchr(";|&<>()`$\n*?[{}", c) != NULL; }

static bool word_break(char c) { return c == ' ' || c == '\t'; }

/* One double-quoted section. Returns 0 ok, -1 when it is unterminated or
 * contains an expansion the shell would perform. */
static int scan_double(const char **cursor, buf_t *word) {
    const char *p = *cursor + 1;
    while (*p && *p != '"') {
        if (*p == '\\' && (p[1] == '\\' || p[1] == '"' || p[1] == '$' || p[1] == '`')) {
            buf_append(word, p + 1, 1);
            p += 2;
        } else if (*p == '$' || *p == '`') {
            return -1;
        } else {
            buf_append(word, p, 1);
            p++;
        }
    }
    if (*p != '"') return -1;
    *cursor = p + 1;
    return 0;
}

static int scan_single(const char **cursor, buf_t *word) {
    const char *p = *cursor + 1;
    const char *end = strchr(p, '\'');
    if (!end) return -1;
    buf_append(word, p, (size_t)(end - p));
    *cursor = end + 1;
    return 0;
}

/* A leading `~` expands only when the word is `~` or starts `~/`; anywhere
 * else the shell leaves it alone, and so do we. */
static bool tilde_expands(const char *p, size_t word_len) {
    if (*p != '~' || word_len) return false;
    return p[1] && p[1] != '/' && !word_break(p[1]);
}

int tny_shellwords(const char *input, tny_words *out) {
    memset(out, 0, sizeof *out);
    if (!input) return -1;
    out->argv = calloc(TNY_WORDS_MAX + 1, sizeof *out->argv);
    out->quoted = calloc(TNY_WORDS_MAX, sizeof *out->quoted);
    if (!out->argv || !out->quoted) return -1;

    const char *p = input;
    int status = 0;
    for (;;) {
        while (word_break(*p)) p++;
        if (!*p || tny_shellword_meta(*p) || tilde_expands(p, 0)) break;
        if (out->argc >= TNY_WORDS_MAX) {
            status = -1;
            break;
        }
        buf_t word;
        buf_init(&word);
        bool quoted = false;
        while (*p && !word_break(*p) && !tny_shellword_meta(*p) && !tilde_expands(p, word.len)) {
            if (*p == '\'') {
                quoted = true;
                status = scan_single(&p, &word);
            } else if (*p == '"') {
                quoted = true;
                status = scan_double(&p, &word);
            } else if (*p == '\\') {
                if (!p[1]) status = -1;
                else {
                    quoted = true;
                    buf_append(&word, p + 1, 1);
                    p += 2;
                }
            } else {
                buf_append(&word, p, 1);
                p++;
            }
            if (status != 0) break;
        }
        if (status != 0 || buf_oom(&word)) {
            buf_free(&word);
            status = -1;
            break;
        }
        out->quoted[out->argc] = quoted;
        out->argv[out->argc] = buf_detach(&word);
        if (!out->argv[out->argc]) {
            status = -1;
            break;
        }
        out->argc++;
    }
    if (status != 0) {
        tny_shellwords_free(out);
        return -1;
    }
    out->consumed = (size_t)(p - input);
    out->stop = *p;
    return 0;
}

void tny_shellwords_free(tny_words *w) {
    if (!w) return;
    if (w->argv)
        for (int i = 0; i < w->argc; i++) free(w->argv[i]);
    free(w->argv);
    free(w->quoted);
    memset(w, 0, sizeof *w);
}
