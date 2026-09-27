/* Python code cells in a fresh, confined child process (docs/adr/0179).
 *
 * Parent (the caller, normally the execution server): starts `--code-cell`
 * with an empty environment and one private socket, sends the trusted catalog
 * and the untrusted source, then answers the child's nested-call frames. It
 * re-checks every frame (tny_code_frame_admit/tny_code_call_admit) because the
 * child is untrusted, owns the deadline (re-read after each callback so host
 * prompt waits can extend it) and kills the child when it passes. Any protocol
 * violation, crash or early exit is an error; nothing is retried or replayed.
 *
 * Child: validates fd 3, initializes the embedded interpreter, drops to
 * resource limits, installs the OS sandbox, and only then reads the source. It
 * exits immediately after its final frame, so no finalizer runs afterwards. */
#include "core/code_runtime.h"
#include "core/code_policy.h"
#include "core/code_python.h"
#include "util/code_sandbox.h"
#include "util/execution_host.h"
#include "util/util.h"
#include "yyjson.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bounded wait for startup, the final frame and EOF after authority ends. */
#define CELL_SETTLE_MS 1000

static bool deadline_passed(void *ud) { return monotonic_ms() >= *(const int64_t *)ud; }

static char *cell_error(const char *reason) {
    size_t len = strlen(reason) + 14;
    char *out = malloc(len);
    if (out) snprintf(out, len, "error: code: %s", reason);
    return out;
}

/* Frames are C strings led by a type byte. */
static int send_frame(int fd, char type, const char *a, size_t alen, const char *b, size_t blen,
                      int64_t deadline, tny_exec_cancel_fn cancel, void *ud) {
    char *frame = malloc(1 + alen + blen + 1);
    if (!frame) {
        errno = ENOMEM;
        return -1;
    }
    frame[0] = type;
    if (alen) memcpy(frame + 1, a, alen);
    if (blen) memcpy(frame + 1 + alen, b, blen);
    frame[1 + alen + blen] = 0;
    int rc = tny_exec_host_send(fd, frame, deadline, cancel, ud);
    free(frame);
    return rc;
}

static bool is_json_object(const char *text, size_t len) {
    yyjson_doc *doc = yyjson_read(text, len, 0);
    bool object = doc && yyjson_is_obj(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return object;
}

char *tny_code_run_with_deadline(const char *code, const int64_t *deadline,
                                 const char *catalog_json, tny_code_call_fn call, void *userdata) {
    if (!code || !tny_code_source_admit(strlen(code))) return cell_error("source limit exceeded");
    if (!deadline) return cell_error("missing host deadline");
    const char *catalog = catalog_json ? catalog_json : "[]";
    tny_exec_host host;
    int rc = tny_exec_host_start_cell(&host);
    if (rc)
        return cell_error(rc == ENOTSUP ? "Python code cells are unsupported on this host"
                                        : "could not start the Python code cell");
    char header[32];
    int header_len = snprintf(header, sizeof header, "%zu:", strlen(catalog));
    size_t catalog_len = strlen(catalog), code_len = strlen(code);
    char *start = malloc((size_t)header_len + catalog_len + 1);
    if (start) {
        memcpy(start, header, (size_t)header_len);
        memcpy(start + header_len, catalog, catalog_len + 1);
    }
    char *output = NULL;
    const char *failure = NULL; /* parent-decided terminal state */
    const char *error = NULL;
    uint64_t calls = 0;
    int phase = TNY_CODE_PHASE_RUNNING;
    if (!start || send_frame(host.fd, TNY_CODE_FRAME_START, start, (size_t)header_len + catalog_len,
                             code, code_len, *deadline, deadline_passed, (void *)deadline))
        error = monotonic_ms() >= *deadline ? "deadline exceeded" : "could not start the cell";
    free(start);
    while (!error && phase == TNY_CODE_PHASE_RUNNING) {
        char *frame = tny_exec_host_receive(host.fd, *deadline, deadline_passed, (void *)deadline);
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
            output = failure ? cell_error(failure) : xstrdup(frame + 1);
            if (!output) error = "result allocation failed";
        } else {
            char *name = frame + 1;
            char *newline = strchr(name, '\n');
            char *arguments = newline ? newline + 1 : NULL;
            size_t name_len = newline ? (size_t)(newline - name) : 0;
            size_t args_len = arguments ? len - (size_t)(arguments - frame) : 0;
            if (newline) *newline = 0;
            if (!newline || !tny_code_call_admit(calls, name_len, strcmp(name, "run_code") == 0,
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
                                   *deadline, deadline_passed, (void *)deadline))
                        error = failure;
                } else {
                    if (send_frame(host.fd, TNY_CODE_FRAME_RESULT, result, strlen(result), NULL, 0,
                                   *deadline, deadline_passed, (void *)deadline))
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
        if (tny_exec_host_expect_eof(host.fd, settle, NULL, NULL))
            error = "cell protocol violation";
    }
    if (error) {
        free(output);
        output = cell_error(error);
        (void)tny_exec_host_kill(&host);
    } else (void)tny_exec_host_close(&host, true);
    return output;
}

char *tny_code_run(const char *code, int timeout_ms, const char *catalog_json,
                   tny_code_call_fn call, void *userdata) {
    if (timeout_ms <= 0) timeout_ms = TNY_CODE_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > TNY_CODE_MAX_TIMEOUT_MS) timeout_ms = TNY_CODE_MAX_TIMEOUT_MS;
    int64_t deadline = monotonic_ms() + timeout_ms;
    return tny_code_run_with_deadline(code, &deadline, catalog_json, call, userdata);
}

/* Child ------------------------------------------------------------------ */

typedef struct {
    int fd;
    char failure[128];
} cell_child;

/* The parent owns time; the child only bounds its wait for a vanished parent. */
static int64_t child_wait(void) {
    return monotonic_ms() + TNY_CODE_MAX_TIMEOUT_MS + 5 * 60 * 1000 + CELL_SETTLE_MS;
}

static char *child_call(void *ud, const char *name, const char *arguments) {
    cell_child *c = ud;
    size_t name_len = strlen(name), args_len = strlen(arguments);
    char *payload = malloc(name_len + 1 + args_len + 1);
    int rc = -1;
    if (payload) {
        snprintf(payload, name_len + 1 + args_len + 1, "%s\n%s", name, arguments);
        rc = send_frame(c->fd, TNY_CODE_FRAME_CALL, payload, name_len + 1 + args_len, NULL, 0,
                        child_wait(), NULL, NULL);
        free(payload);
    }
    char *reply = rc ? NULL : tny_exec_host_receive(c->fd, child_wait(), NULL, NULL);
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

int tny_code_cell_main(void) {
    int fd = tny_exec_host_accept();
    if (fd < 0) return 2;
    cell_child child = {.fd = fd};
    const char *refusal = NULL;
    /* Trusted setup first (interpreter startup reads its own configuration),
     * then limits and the sandbox, and only then the untrusted source. */
    if (!tny_code_python_available() || tny_code_python_init())
        refusal = "Python runtime initialization failed";
    else if (tny_code_sandbox_limits(TNY_CODE_MAX_TIMEOUT_MS / 1000 + 15))
        refusal = "could not apply code-cell resource limits";
    else if (tny_code_sandbox_enter())
        refusal = errno == ENOTSUP ? "no OS sandbox for code cells on this host"
                                   : "could not enter the code-cell OS sandbox";
    char *start = tny_exec_host_receive(fd, child_wait(), NULL, NULL);
    char *output = NULL;
    char *colon = start && start[0] == TNY_CODE_FRAME_START ? strchr(start + 1, ':') : NULL;
    char *end = NULL;
    unsigned long long catalog_len = colon ? strtoull(start + 1, &end, 10) : 0;
    if (refusal) output = cell_error(refusal);
    else if (!colon || end != colon || catalog_len > strlen(colon + 1))
        output = cell_error("cell protocol violation");
    else {
        const char *catalog = colon + 1;
        const char *code = catalog + catalog_len;
        char *catalog_copy = malloc(catalog_len + 1);
        if (catalog_copy) {
            memcpy(catalog_copy, catalog, catalog_len);
            catalog_copy[catalog_len] = 0;
            tny_code_python_host host = {.catalog = catalog_copy,
                                         .call = child_call,
                                         .userdata = &child,
                                         .failure = child_failure};
            output = tny_code_python_run(code, &host);
            free(catalog_copy);
        }
    }
    free(start);
    if (!output) output = cell_error("result allocation failed");
    int rc = output ? send_frame(fd, TNY_CODE_FRAME_DONE, output, strlen(output), NULL, 0,
                                 child_wait(), NULL, NULL)
                    : -1;
    free(output);
    /* No interpreter finalization: nothing may run after the final frame. */
    _exit(rc ? 1 : 0);
}
