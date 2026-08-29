#include "session/session.h"
#include "util/tt.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TT_SHUTDOWN_GRACE_MS 100

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
    free(s->pending);
    free(s);
}

static void session_stop(tt_session *s) {
    tt_pty_kill(&s->pty);
    tt_pty_close(&s->pty);
    int waited = 0;
    while (s->pty.pid > 0 && waited < TT_SHUTDOWN_GRACE_MS) {
        if (tt_pty_reap(&s->pty, &s->exit_code, false) != 0) break;
        poll(NULL, 0, 10);
        waited += 10;
    }
    if (s->pty.pid > 0) {
        tt_pty_force_kill(&s->pty);
        tt_pty_reap(&s->pty, &s->exit_code, true);
    }
}

void tt_session_destroy(tt_registry *r, tt_session *s) {
    tt_session **pp = &r->head;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = s->next;
    r->count--;
    if (s->alive) session_stop(s);
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
    session_stop(s);
    return -1;
}

int tt_session_drain(tt_session *s, char *scratch, size_t scratch_len, int max_reads,
                     void (*raw)(void *user, const char *bytes, size_t len), void *raw_user) {
    int total = 0;
    for (int i = 0; i < max_reads; i++) {
        int n = tt_session_pump(s, scratch, scratch_len, raw, raw_user);
        if (n <= 0) return total > 0 ? total : n;
        total += n;
    }
    return total;
}

size_t tt_session_pending(const tt_session *s) { return s->pend_len - s->pend_off; }

/* Drop the queue: the pty is gone, so its input can never be delivered. */
static void queue_reset(tt_session *s) {
    s->pend_off = 0;
    s->pend_len = 0;
}

/* Append to the tail of the queue, growing it (never past the cap, which
 * the caller has already checked) and reclaiming drained head bytes. */
static int queue_append(tt_session *s, const char *bytes, size_t len) {
    if (s->pend_off == s->pend_len) queue_reset(s);
    if (s->pend_len + len > s->pend_cap) {
        if (s->pend_off > 0) { /* compact before growing */
            memmove(s->pending, s->pending + s->pend_off, s->pend_len - s->pend_off);
            s->pend_len -= s->pend_off;
            s->pend_off = 0;
        }
        if (s->pend_len + len > s->pend_cap) {
            size_t cap = s->pend_cap ? s->pend_cap : 4096;
            while (cap < s->pend_len + len) cap *= 2;
            if (cap > TT_INPUT_QUEUE_MAX) cap = TT_INPUT_QUEUE_MAX;
            char *grown = realloc(s->pending, cap);
            if (!grown) return -1;
            s->pending = grown;
            s->pend_cap = cap;
        }
    }
    memcpy(s->pending + s->pend_len, bytes, len);
    s->pend_len += len;
    return 0;
}

/* Reserve for the worst case before writing any prefix to the pty. This
 * preserves the API's no-partial-delivery rule if allocation fails. */
static int queue_reserve(tt_session *s, size_t len) {
    size_t queued = tt_session_pending(s);
    if (s->pend_off > 0 && s->pend_len + len > s->pend_cap) {
        memmove(s->pending, s->pending + s->pend_off, queued);
        s->pend_off = 0;
        s->pend_len = queued;
    }
    size_t need = s->pend_len + len;
    if (need <= s->pend_cap) return 0;
    size_t cap = s->pend_cap ? s->pend_cap : 4096;
    while (cap < need) cap *= 2;
    if (cap > TT_INPUT_QUEUE_MAX) cap = TT_INPUT_QUEUE_MAX;
    char *grown = realloc(s->pending, cap);
    if (!grown) return -1;
    s->pending = grown;
    s->pend_cap = cap;
    return 0;
}

/* Push bytes at the pty until it says EAGAIN. Returns how many went in,
 * or -1 (session dead) — the caller queues whatever is left. */
static ssize_t pty_push(tt_session *s, const char *bytes, size_t len) {
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
    return (ssize_t)off;
}

int tt_session_flush(tt_session *s) {
    size_t queued = tt_session_pending(s);
    if (queued == 0) return 0;
    if (!s->alive || s->pty.master < 0) {
        queue_reset(s);
        errno = ENXIO;
        return -1;
    }
    ssize_t n = pty_push(s, s->pending + s->pend_off, queued);
    if (n < 0) {
        queue_reset(s);
        return -1;
    }
    s->pend_off += (size_t)n;
    if (s->pend_off == s->pend_len) queue_reset(s);
    return (int)n;
}

int tt_session_write(tt_session *s, const char *bytes, size_t len) {
    if (!s->alive || s->pty.master < 0) {
        errno = ENXIO;
        return -1;
    }
    if (len == 0) return 0;
    /* Worst case every byte is queued; check the cap up front so a
     * rejected write never leaves a torn prefix in the pty. */
    if (len > TT_INPUT_QUEUE_MAX - tt_session_pending(s)) {
        errno = ENOBUFS;
        return -1;
    }
    if (queue_reserve(s, len) != 0) {
        errno = ENOMEM;
        return -1;
    }
    size_t off = 0;
    if (tt_session_pending(s) == 0) { /* nothing owed: the fd may take it now */
        ssize_t n = pty_push(s, bytes, len);
        if (n < 0) return -1;
        off = (size_t)n;
    }
    if (off < len && queue_append(s, bytes + off, len - off) != 0) {
        errno = ENOMEM;
        return -1;
    }
    return (int)len;
}

int tt_session_resize(tt_session *s, int cols, int rows) {
    if (cols < 1 || rows < 1) return -1;
    vt_resize(s->term, cols, rows);
    if (s->alive) return tt_pty_resize(&s->pty, cols, rows);
    return 0;
}

int tt_registry_poll_fill(tt_registry *r, struct pollfd *fds, tt_session **sessions, int max,
                          bool attached) {
    int n = 0;
    for (tt_session *s = r->head; s && n < max; s = s->next) {
        if (!s->alive || s->attached != attached) continue;
        sessions[n] = s;
        fds[n].fd = s->pty.master;
        fds[n].events = POLLIN | (tt_session_pending(s) ? POLLOUT : 0);
        fds[n].revents = 0;
        n++;
    }
    return n;
}

void tt_registry_poll_handle(const struct pollfd *fds, tt_session *const *sessions, int n,
                             char *scratch, size_t scratch_len) {
    for (int i = 0; i < n; i++) {
        tt_session *s = sessions[i];
        if (!s) continue;
        if (fds[i].revents & POLLOUT) tt_session_flush(s);
        if (fds[i].revents & (POLLIN | POLLHUP))
            tt_session_drain(s, scratch, scratch_len, TT_PUMP_READS_PER_TURN, NULL, NULL);
    }
}
