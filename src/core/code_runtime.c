/* Python code cells in a fresh child process with host authority
 * (docs/adr/0180, superseding the confinement of docs/adr/0179).
 *
 * Parent (the caller, normally the execution server): starts `--code-cell`
 * with its own environment, stdout and stderr on a pipe it drains, and one
 * private protocol socket on fd 3. It sends the trusted catalog, the working
 * directory and the source, then answers nested-call frames. It re-checks
 * every frame (tny_code_frame_admit/tny_code_call_admit), owns the deadline
 * (re-read after each callback so host prompt waits can extend it) and stops
 * the child and its owned descendants when it passes. Any protocol
 * violation, crash or early exit is an error; nothing is retried or replayed.
 *
 * Captured output is bounded here, not in the child: whatever the cell, its
 * imported modules or its subprocesses write to fds 1/2 keeps the first bytes
 * and the last CELL_OUTPUT_TAIL bytes, with the omitted count between them.
 *
 * Child: validates fd 3, reads the start frame, enters the working directory,
 * initializes the embedded interpreter and runs the source with the OS user's
 * ordinary authority. It exits immediately after its final frame, so no
 * finalizer runs afterwards. A fork() child of the cell loses the protocol
 * socket; exec'd programs never receive it (FD_CLOEXEC). */
#include "core/code_runtime.h"
#include "core/code_policy.h"
#include "core/code_python.h"
#include "util/execution_host.h"
#include "util/util.h"
#include "yyjson.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <pthread.h>
#endif

/* Bounded wait for startup, the final frame and EOF after authority ends. */
#define CELL_SETTLE_MS 1000
/* Captured output beyond TNY_CODE_OUTPUT_BYTES keeps this much of its end. */
#define CELL_OUTPUT_TAIL (16u * 1024u)
/* One drain call reads at most this much before the frame wait resumes. */
#define CELL_DRAIN_BYTES (1024u * 1024u)

static bool deadline_passed(void *ud) { return monotonic_ms() >= *(const int64_t *)ud; }

static char *cell_error(const char *reason) {
    size_t len = strlen(reason) + 14;
    char *out = malloc(len);
    if (out) snprintf(out, len, "error: code: %s", reason);
    return out;
}

/* Captured stdout/stderr ---------------------------------------------------- */

typedef struct {
    tny_exec_aux aux; /* read end of the cell's stdout/stderr pipe */
    char head[TNY_CODE_OUTPUT_BYTES];
    size_t head_len;
    char tail[CELL_OUTPUT_TAIL]; /* ring: the latest bytes of the whole stream */
    size_t tail_next;
    uint64_t total;
} cell_output;

static void output_append(cell_output *o, const char *data, size_t len) {
    uint64_t take = tny_code_output_take(o->head_len, len);
    memcpy(o->head + o->head_len, data, (size_t)take);
    o->head_len += (size_t)take;
    if (len >= CELL_OUTPUT_TAIL) {
        memcpy(o->tail, data + len - CELL_OUTPUT_TAIL, CELL_OUTPUT_TAIL);
        o->tail_next = 0;
    } else {
        size_t first =
            CELL_OUTPUT_TAIL - o->tail_next < len ? CELL_OUTPUT_TAIL - o->tail_next : len;
        memcpy(o->tail + o->tail_next, data, first);
        memcpy(o->tail, data + first, len - first);
        o->tail_next = (o->tail_next + len) % CELL_OUTPUT_TAIL;
    }
    o->total += len;
}

static void output_drain(void *ud) {
    cell_output *o = ud;
    char chunk[16384];
    for (size_t read_bytes = 0; o->aux.fd >= 0 && read_bytes < CELL_DRAIN_BYTES;) {
        ssize_t n = read(o->aux.fd, chunk, sizeof chunk);
        if (n > 0) {
            output_append(o, chunk, (size_t)n);
            read_bytes += (size_t)n;
        } else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        else {
            /* EOF: every writer, including background descendants, closed. */
            close(o->aux.fd);
            o->aux.fd = -1;
        }
    }
}

/* Valid UTF-8 for the JSON result: NUL and malformed bytes become U+FFFD. */
static void append_utf8(buf_t *b, const unsigned char *s, size_t n) {
    static const char replacement[] = "\xEF\xBF\xBD";
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t need = (size_t)tny_code_utf8_lead_width(c);
        bool ok = need && need <= n - i;
        for (size_t k = 1; ok && k < need; ++k) ok = (s[i + k] & 0xC0) == 0x80;
        if (ok && need == 3)
            ok = !(c == 0xE0 && s[i + 1] < 0xA0) && !(c == 0xED && s[i + 1] >= 0xA0);
        if (ok && need == 4)
            ok = !(c == 0xF0 && s[i + 1] < 0x90) && !(c == 0xF4 && s[i + 1] >= 0x90);
        if (ok) {
            buf_append(b, s + i, need);
            i += need;
        } else {
            buf_append(b, replacement, 3);
            ++i;
        }
    }
}

/* The captured text, at most `room` bytes: everything when it fits, otherwise
 * the first bytes, an omission marker and the last bytes. Head and tail
 * cannot overlap: together they are shorter than room < total. */
static void output_text(const cell_output *o, buf_t *b, size_t room) {
    buf_t raw;
    buf_init(&raw);
    if (o->total <= room) buf_append(&raw, o->head, o->head_len);
    else {
        size_t ring = o->total < CELL_OUTPUT_TAIL ? (size_t)o->total : CELL_OUTPUT_TAIL;
        size_t keep_tail = ring < room / 4 ? ring : room / 4;
        size_t keep_head = room > keep_tail + 64 ? room - keep_tail - 64 : 0;
        if (keep_head > o->head_len) keep_head = o->head_len;
        buf_append(&raw, o->head, keep_head);
        buf_appendf(&raw, "\n[tny: %llu bytes of output omitted]\n",
                    (unsigned long long)(o->total - keep_head - keep_tail));
        size_t from = (o->tail_next + CELL_OUTPUT_TAIL - keep_tail) % CELL_OUTPUT_TAIL;
        size_t first = CELL_OUTPUT_TAIL - from < keep_tail ? CELL_OUTPUT_TAIL - from : keep_tail;
        buf_append(&raw, o->tail + from, first);
        buf_append(&raw, o->tail, keep_tail - first);
    }
    size_t start = b->len;
    if (raw.data) append_utf8(b, (const unsigned char *)raw.data, raw.len);
    buf_free(&raw);
    /* Replacement characters can only grow invalid input; cut on a character. */
    if (b->len - start > room && !b->oom) {
        size_t cut = start + room;
        while (cut > start && ((unsigned char)b->data[cut] & 0xC0) == 0x80) --cut;
        b->len = cut;
        b->data[cut] = 0;
    }
}

/* The cell result: output alone, or the error line with the output that
 * preceded it (so the model sees which effects already happened). The error
 * line comes first so "error:" classification of the whole result holds. */
static char *cell_result(const cell_output *o, const char *error) {
    static const char separator[] = "\n[output before the error]\n";
    buf_t b;
    buf_init(&b);
    if (!error || !*error) output_text(o, &b, TNY_CODE_OUTPUT_BYTES);
    else {
        size_t elen = strlen(error);
        if (elen > TNY_CODE_RESULT_TEXT_BYTES) elen = TNY_CODE_RESULT_TEXT_BYTES;
        append_utf8(&b, (const unsigned char *)error, elen);
        if (b.len > TNY_CODE_RESULT_TEXT_BYTES && !b.oom) {
            size_t cut = TNY_CODE_RESULT_TEXT_BYTES;
            while (cut && ((unsigned char)b.data[cut] & 0xC0) == 0x80) --cut;
            b.len = cut;
            b.data[cut] = 0;
        }
        size_t used = b.len + sizeof separator - 1;
        if (o->total && used < TNY_CODE_RESULT_TEXT_BYTES) {
            buf_appends(&b, separator);
            size_t room = TNY_CODE_RESULT_TEXT_BYTES - used;
            output_text(o, &b, room < TNY_CODE_OUTPUT_BYTES ? room : TNY_CODE_OUTPUT_BYTES);
        }
    }
    return buf_detach(&b);
}

/* Parent ------------------------------------------------------------------- */

/* Frames are C strings led by a type byte. */
static int send_frame(int fd, char type, const char *a, size_t alen, const char *b, size_t blen,
                      tny_exec_aux *aux, int64_t deadline, tny_exec_cancel_fn cancel, void *ud) {
    char *frame = malloc(1 + alen + blen + 1);
    if (!frame) {
        errno = ENOMEM;
        return -1;
    }
    frame[0] = type;
    if (alen) memcpy(frame + 1, a, alen);
    if (blen) memcpy(frame + 1 + alen, b, blen);
    frame[1 + alen + blen] = 0;
    int rc = tny_exec_host_send_aux(fd, frame, aux, deadline, cancel, ud);
    free(frame);
    return rc;
}

static bool is_json_object(const char *text, size_t len) {
    yyjson_doc *doc = yyjson_read(text, len, 0);
    bool object = doc && yyjson_is_obj(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return object;
}

#ifndef __EMSCRIPTEN__
/* Pipe for the child's stdout/stderr: the parent keeps a nonblocking read end;
 * both ends are close-on-exec (the spawn maps a staged copy of the write end). */
static int output_pipe(int fds[2]) {
    if (pipe(fds)) return -1;
    for (int i = 0; i < 2; ++i)
        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
    int flags = fcntl(fds[0], F_GETFL);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    return 0;
fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
}
#endif

static char *run_cell(cell_output *out, const char *code, const char *cwd, const int64_t *deadline,
                      const char *catalog, tny_code_call_fn call, void *userdata) {
    tny_exec_aux *aux = &out->aux;
    int fds[2] = {-1, -1};
#ifdef __EMSCRIPTEN__
    int rc = ENOTSUP;
#else
    int rc = output_pipe(fds) ? errno : 0;
#endif
    tny_exec_host host = {.fd = -1, .pid = -1};
    if (!rc) rc = tny_exec_host_start_cell(&host, fds[1]);
    /* Only the child may hold the write end, or EOF could never arrive. */
    if (fds[1] >= 0) close(fds[1]);
    if (rc) {
        if (fds[0] >= 0) close(fds[0]);
        return cell_error(rc == ENOTSUP ? "Python code cells are unsupported on this host"
                                        : "could not start the Python code cell");
    }
    out->aux = (tny_exec_aux){.fd = fds[0], .drain = output_drain, .ud = out};
    size_t catalog_len = strlen(catalog), cwd_len = cwd ? strlen(cwd) : 0, code_len = strlen(code);
    char header[48];
    int header_len = snprintf(header, sizeof header, "%zu:%zu:", catalog_len, cwd_len);
    char *start = malloc((size_t)header_len + catalog_len + cwd_len + 1);
    if (start) {
        memcpy(start, header, (size_t)header_len);
        memcpy(start + header_len, catalog, catalog_len);
        if (cwd_len) memcpy(start + header_len + catalog_len, cwd, cwd_len);
        start[header_len + catalog_len + cwd_len] = 0;
    }
    char *done = NULL;          /* the child's final summary: "" or an error line */
    const char *failure = NULL; /* parent-decided terminal state */
    const char *error = NULL;
    uint64_t calls = 0;
    int phase = TNY_CODE_PHASE_RUNNING;
    if (!start ||
        send_frame(host.fd, TNY_CODE_FRAME_START, start, (size_t)header_len + catalog_len + cwd_len,
                   code, code_len, aux, *deadline, deadline_passed, (void *)deadline))
        error = monotonic_ms() >= *deadline ? "deadline exceeded" : "could not start the cell";
    free(start);
    while (!error && phase == TNY_CODE_PHASE_RUNNING) {
        char *frame =
            tny_exec_host_receive_aux(host.fd, aux, *deadline, deadline_passed, (void *)deadline);
        if (!frame) {
            error = monotonic_ms() >= *deadline ? "deadline exceeded"
                    : errno == EMSGSIZE         ? "cell protocol violation"
                                                : "Python runtime exited unexpectedly";
            break;
        }
        size_t len = strlen(frame);
        int type = (unsigned char)frame[0];
        if (!tny_code_frame_admit(phase, type, len, calls)) {
            error = "cell protocol violation";
        } else if (type == TNY_CODE_FRAME_DONE) {
            phase = TNY_CODE_PHASE_FINISHED;
            done = failure ? cell_error(failure) : xstrdup(frame + 1);
            if (!done) error = "result allocation failed";
        } else {
            /* A fixed three-digit decimal byte length makes the name exact,
             * including punctuation/newlines. Never reinterpret part of a
             * name as JSON whitespace or scan a missing argument pointer. */
            bool prefix = len >= 4 && frame[1] >= '0' && frame[1] <= '9' && frame[2] >= '0' &&
                          frame[2] <= '9' && frame[3] >= '0' && frame[3] <= '9';
            size_t name_len = prefix ? (size_t)(frame[1] - '0') * 100 +
                                           (size_t)(frame[2] - '0') * 10 + (size_t)(frame[3] - '0')
                                     : 0;
            char name[TNY_CODE_NAME_BYTES + 1] = {0};
            bool complete =
                prefix && name_len >= 1 && name_len <= TNY_CODE_NAME_BYTES && len >= 4 + name_len;
            const char *arguments = complete ? frame + 4 + name_len : NULL;
            size_t args_len = complete ? len - 4 - name_len : 0;
            if (complete) memcpy(name, frame + 4, name_len);
            if (!complete || !tny_code_call_admit(calls, name_len, strcmp(name, "run_code") == 0,
                                                  args_len, is_json_object(arguments, args_len))) {
                error = "cell protocol violation";
            } else {
                ++calls;
                char *result = call ? call(userdata, name, arguments) : NULL;
                if (monotonic_ms() >= *deadline) {
                    free(result);
                    error = "deadline exceeded";
                } else if (!result || !tny_code_result_admit(strlen(result))) {
                    failure = result ? "tool result limit exceeded" : "tool callback failed";
                    free(result);
                    /* Terminal: the spent budget makes any further call frame
                     * inadmissible; only the child's final frame may follow. */
                    calls = TNY_CODE_TOOL_CALLS;
                    if (send_frame(host.fd, TNY_CODE_FRAME_FAIL, failure, strlen(failure), NULL, 0,
                                   aux, *deadline, deadline_passed, (void *)deadline))
                        error = failure;
                } else {
                    if (send_frame(host.fd, TNY_CODE_FRAME_RESULT, result, strlen(result), NULL, 0,
                                   aux, *deadline, deadline_passed, (void *)deadline))
                        error = monotonic_ms() >= *deadline ? "deadline exceeded"
                                                            : "Python runtime exited unexpectedly";
                    free(result);
                }
            }
        }
        free(frame);
    }
    if (!error && phase == TNY_CODE_PHASE_FINISHED) {
        /* Authority ended with the final frame; the child must now just exit. */
        int64_t settle = monotonic_ms() + CELL_SETTLE_MS;
        if (tny_exec_host_expect_eof_aux(host.fd, aux, settle, NULL, NULL))
            error = "cell protocol violation";
    }
    if (error) (void)tny_exec_host_kill(&host);
    else if (tny_exec_host_close(&host, true)) {
        error = "Python runtime did not exit successfully";
        /* Reap failures retain the existing generation-safe cleanup boundary. */
        (void)tny_exec_host_kill(&host);
    }
    /* Whatever the cell wrote before it exited is already in the pipe. Later
     * writers are background descendants; the result does not wait for them. */
    if (aux->fd >= 0) {
        output_drain(out);
        if (aux->fd >= 0) close(aux->fd);
        aux->fd = -1;
    }
    char *text = NULL;
    if (error) {
        char *line = cell_error(error);
        text = line ? cell_result(out, line) : NULL;
        free(line);
    } else text = cell_result(out, done);
    free(done);
    return text;
}

char *tny_code_run_with_deadline(const char *code, const char *cwd, const int64_t *deadline,
                                 const char *catalog_json, tny_code_call_fn call, void *userdata) {
    if (!code || !tny_code_source_admit(strlen(code))) return cell_error("source limit exceeded");
    if (!deadline) return cell_error("missing host deadline");
    if (cwd && (!*cwd || strlen(cwd) > TNY_CODE_SOURCE_BYTES))
        return cell_error("invalid working directory");
    cell_output *out = calloc(1, sizeof *out);
    if (!out) return NULL;
    out->aux.fd = -1;
    char *text =
        run_cell(out, code, cwd, deadline, catalog_json ? catalog_json : "[]", call, userdata);
    free(out);
    return text;
}

char *tny_code_run(const char *code, int timeout_ms, const char *catalog_json,
                   tny_code_call_fn call, void *userdata) {
    if (timeout_ms <= 0) timeout_ms = TNY_CODE_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > TNY_CODE_MAX_TIMEOUT_MS) timeout_ms = TNY_CODE_MAX_TIMEOUT_MS;
    int64_t deadline = monotonic_ms() + timeout_ms;
    return tny_code_run_with_deadline(code, NULL, &deadline, catalog_json, call, userdata);
}

/* Child ------------------------------------------------------------------ */

typedef struct {
    int fd; /* protocol socket; -1 in a fork() child of the cell */
    char failure[128];
} cell_child;

#ifndef __EMSCRIPTEN__
static cell_child *forked_owner;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;

/* A fork() child keeps running Python but is not the cell: it must neither
 * hold the parent's EOF open nor ever write a frame. close() is
 * async-signal-safe; the descriptor number is invalidated so a reused fd 3
 * can never receive protocol bytes. */
static void forked_child(void) {
    if (forked_owner && forked_owner->fd >= 0) {
        close(forked_owner->fd);
        forked_owner->fd = -1;
    }
}
#endif

/* The parent owns time; the child only bounds its wait for a vanished parent. */
static int64_t child_wait(void) {
    return monotonic_ms() + TNY_CODE_MAX_TIMEOUT_MS + 5 * 60 * 1000 + CELL_SETTLE_MS;
}

/* Frames leave whole even if another Python thread trips a heap limit. */
static int child_send(cell_child *c, const char *frame) {
    if (c->fd < 0) {
        errno = EBADF;
        return -1;
    }
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&frame_lock);
#endif
    int rc = tny_exec_host_send(c->fd, frame, child_wait(), NULL, NULL);
#ifndef __EMSCRIPTEN__
    pthread_mutex_unlock(&frame_lock);
#endif
    return rc;
}

static char *child_call(void *ud, const char *name, const char *arguments) {
    cell_child *c = ud;
    size_t name_len = strlen(name), args_len = strlen(arguments);
    char *frame = malloc(1 + 3 + name_len + args_len + 1);
    int rc = -1;
    if (frame) {
        frame[0] = TNY_CODE_FRAME_CALL;
        int prefix = snprintf(frame + 1, 4, "%03u", (unsigned)name_len);
        if (prefix == 3 && name_len >= 1 && name_len <= TNY_CODE_NAME_BYTES) {
            memcpy(frame + 4, name, name_len);
            memcpy(frame + 4 + name_len, arguments, args_len + 1);
            rc = child_send(c, frame);
        }
        free(frame);
    }
    char *reply = rc || c->fd < 0 ? NULL : tny_exec_host_receive(c->fd, child_wait(), NULL, NULL);
    char *result = NULL;
    if (reply && reply[0] == TNY_CODE_FRAME_RESULT) {
        size_t len = strlen(reply + 1);
        result = malloc(len + 1);
        if (result) memcpy(result, reply + 1, len + 1);
        else snprintf(c->failure, sizeof c->failure, "tool result allocation failed");
    } else if (reply && reply[0] == TNY_CODE_FRAME_FAIL)
        snprintf(c->failure, sizeof c->failure, "%s", reply + 1);
    else
        snprintf(c->failure, sizeof c->failure, "%s",
                 reply ? "cell protocol violation" : "cell transport failed");
    free(reply);
    return result;
}

static const char *child_failure(void *ud) {
    cell_child *c = ud;
    return c->failure[0] ? c->failure : NULL;
}

/* Quotas are a process-terminal event, not a catchable Python exception.
 * The existing DONE frame settles a bounded error; _exit prevents any later
 * bytecode, finalizer or raw-IPC alias from asking the parent for more effects.
 * The transport uses only stack storage and bounded I/O, even under heap OOM. */
static _Noreturn void child_abort(void *ud, const char *reason) {
    cell_child *c = ud;
    char frame[256];
    frame[0] = TNY_CODE_FRAME_DONE;
    int n = snprintf(frame + 1, sizeof frame - 1, "error: code: %s",
                     reason ? reason : "terminal execution failure");
    int rc = n < 0 ? -1 : child_send(c, frame);
    _exit(rc ? 1 : 0);
}

/* "S" catalog_len ":" cwd_len ":" catalog cwd code */
static bool parse_start(char *start, const char **catalog, size_t *catalog_len, char **cwd,
                        const char **code) {
    if (!start || start[0] != TNY_CODE_FRAME_START) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long a = strtoull(start + 1, &end, 10);
    if (errno || end == start + 1 || *end != ':') return false;
    char *second = end + 1;
    unsigned long long b = strtoull(second, &end, 10);
    if (errno || end == second || *end != ':') return false;
    const char *body = end + 1;
    size_t body_len = strlen(body);
    if (a > body_len || b > body_len - a) return false;
    *catalog = body;
    *catalog_len = (size_t)a;
    *cwd = NULL;
    if (b) {
        *cwd = malloc((size_t)b + 1);
        if (!*cwd) return false;
        memcpy(*cwd, body + a, (size_t)b);
        (*cwd)[b] = 0;
    }
    *code = body + a + b;
    return true;
}

int tny_code_cell_main(void) {
    int fd = tny_exec_host_accept();
    if (fd < 0) return 2;
    cell_child child = {.fd = fd};
#ifndef __EMSCRIPTEN__
    forked_owner = &child;
    if (pthread_atfork(NULL, NULL, forked_child)) return 2;
#endif
    char *start = tny_exec_host_receive(fd, child_wait(), NULL, NULL);
    const char *catalog = NULL, *code = NULL;
    size_t catalog_len = 0;
    char *cwd = NULL;
    char *output = NULL;
    if (!parse_start(start, &catalog, &catalog_len, &cwd, &code))
        output = cell_error("cell protocol violation");
    else if (cwd && chdir(cwd)) {
        char reason[160];
        snprintf(reason, sizeof reason, "could not enter the working directory: %s",
                 strerror(errno));
        output = cell_error(reason);
    } else {
        /* Shells and tools trust PWD when it names the working directory. */
        if (cwd) (void)setenv("PWD", cwd, 1);
        if (!tny_code_python_available() || tny_code_python_init())
            output = cell_error("Python runtime initialization failed");
        else {
            char *catalog_copy = malloc(catalog_len + 1);
            if (catalog_copy) {
                memcpy(catalog_copy, catalog, catalog_len);
                catalog_copy[catalog_len] = 0;
                tny_code_python_host host = {.catalog = catalog_copy,
                                             .call = child_call,
                                             .userdata = &child,
                                             .failure = child_failure,
                                             .abort = child_abort};
                output = tny_code_python_run(code, &host);
                free(catalog_copy);
            }
        }
    }
    free(cwd);
    free(start);
    if (!output) output = cell_error("result allocation failed");
    int rc = -1;
    char *frame = output ? malloc(strlen(output) + 2) : NULL;
    if (frame) {
        frame[0] = TNY_CODE_FRAME_DONE;
        memcpy(frame + 1, output, strlen(output) + 1);
        rc = child_send(&child, frame);
    }
    free(frame);
    free(output);
    /* No interpreter finalization: nothing may run after the final frame. */
    _exit(rc ? 1 : 0);
}
