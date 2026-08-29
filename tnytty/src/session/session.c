#include "session/session.h"
#include "util/tt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void tt_registry_init(tt_registry *r, int scrollback) {
    r->head = NULL;
    r->count = 0;
    r->scrollback = scrollback;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (char **a = argv; *a; a++) free(*a);
    free(argv);
}

static char **copy_argv(char *const argv[]) {
    static const char *fallback = "/bin/sh";
    const char *shell_argv[2] = {NULL, NULL};
    if (!argv || !argv[0]) {
        const char *sh = getenv("SHELL");
        shell_argv[0] = sh && *sh ? sh : fallback;
        argv = (char *const *)shell_argv;
    }
    int n = 0;
    while (argv[n]) n++;
    char **copy = calloc((size_t)n + 1, sizeof *copy);
    if (!copy) return NULL;
    for (int i = 0; i < n; i++) {
        copy[i] = strdup(argv[i]);
        if (!copy[i]) {
            free_argv(copy);
            return NULL;
        }
    }
    return copy;
}

tt_session *tt_session_create(tt_registry *r, char *const argv[], int cols, int rows) {
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    tt_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->argv = copy_argv(argv);
    s->term = vt_new(cols, rows, r->scrollback);
    if (!s->argv || !s->term) goto fail;
    tt_rand_hex(s->id, TT_SESSION_ID_LEN);
    if (tt_pty_spawn(&s->pty, s->argv, cols, rows) != 0) goto fail;
    s->alive = true;
    s->created = time(NULL);
    s->next = r->head;
    r->head = s;
    r->count++;
    return s;
fail: {
    int saved = errno;
    vt_free(s->term);
    free_argv(s->argv);
    free(s);
    errno = saved;
    return NULL;
}
}

tt_session *tt_session_find(tt_registry *r, const char *id) {
    for (tt_session *s = r->head; s; s = s->next)
        if (strcmp(s->id, id) == 0) return s;
    return NULL;
}

static void session_free(tt_session *s) {
    tt_pty_close(&s->pty);
    vt_free(s->term);
    free_argv(s->argv);
    free(s);
}

void tt_session_destroy(tt_registry *r, tt_session *s) {
    tt_session **pp = &r->head;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = s->next;
    r->count--;
    if (s->alive) {
        tt_pty_kill(&s->pty);
        tt_pty_close(&s->pty);
        tt_pty_reap(&s->pty, &s->exit_code, true);
    }
    session_free(s);
}

void tt_registry_free(tt_registry *r) {
    while (r->head) tt_session_destroy(r, r->head);
}

int tt_session_pump(tt_session *s, char *scratch, size_t scratch_len,
                    void (*raw)(void *user, const char *bytes, size_t len), void *raw_user) {
    if (!s->alive || s->pty.master < 0) return -1;
    ssize_t n = read(s->pty.master, scratch, scratch_len);
    if (n > 0) {
        vt_feed(s->term, scratch, (size_t)n);
        if (raw) raw(raw_user, scratch, (size_t)n);
        return (int)n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    /* EOF (or EIO on Linux when the slave side closed): child is gone. */
    s->alive = false;
    tt_pty_close(&s->pty);
    tt_pty_reap(&s->pty, &s->exit_code, true);
    return -1;
}

int tt_session_write(tt_session *s, const char *bytes, size_t len) {
    if (!s->alive || s->pty.master < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(s->pty.master, bytes + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* pty buffer full */
            return -1;
        }
        off += (size_t)n;
    }
    return (int)off;
}

int tt_session_resize(tt_session *s, int cols, int rows) {
    if (cols < 1 || rows < 1) return -1;
    vt_resize(s->term, cols, rows);
    if (s->alive) return tt_pty_resize(&s->pty, cols, rows);
    return 0;
}
