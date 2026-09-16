/* Retention regression through a live backend, before teardown or a later turn. */
#include "backends/openai/openai.h"
#include "core/config.h"
#include "core/perm.h"
#include "core/session.h"
#include "cpp/testing.h"
#include "net/http_server.h"
#include "util/alloc.h"
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(bool value, const char *reason) {
    if (!value) {
        fprintf(stderr, "backend parser check failed: %s\n", reason);
        abort();
    }
}
typedef struct {
    buf_t wire;
    bool ended, armed, parked;
    int mode; /* OOM, stream cancellation, then parked SSE/JSON tool batches */
    tny_backend *backend;
    int errors, ends, cancelled_tools;
    size_t baseline, before, at_failure;
} fixture;

static int respond(const http_server_request *request, http_server_response *response, void *ud) {
    (void)request;
    fixture *f = ud;
    response->status = 200;
    response->content_type = f->mode == 6 ? "application/json" : "text/event-stream";
    response->body = f->wire.data;
    response->body_len = f->wire.len;
    return HTTP_SERVER_POST_HANDLED;
}
static void event(const tny_backend_event *ev, void *ud) {
    fixture *f = ud;
    if (ev->kind == TNY_EV_PERMISSION && f->mode >= 5) {
        check(!f->parked, "one pending permission before cancellation");
        f->parked = f->armed = true;
        f->before = tny_parser_test_live_allocations();
        check(f->before > f->baseline, "parked batch retains parser allocations");
    }
    if (ev->kind == TNY_EV_TOOL_END && f->mode >= 5) {
        check(!f->ended && !ev->tool_ok, "cancelled tools finish before terminal delivery");
        check(strcmp(ev->tool_id, f->cancelled_tools == 0
                                      ? "retained-call-long-enough-to-allocate"
                                      : "second-call-long-enough-to-allocate") == 0,
              "pending and unstarted call IDs survive cancellation");
        check(strcmp(ev->tool_name, "terminal") == 0 && ev->tool_detail &&
                  strstr(ev->tool_detail, "interrupted before terminal ran"),
              "each retained call receives its cancellation result");
        f->cancelled_tools++;
    }
    if (ev->kind == TNY_EV_TEXT_DELTA && !f->armed && f->mode < 5) {
        f->before = tny_parser_test_live_allocations();
        check(f->before > 3, "pending calls and stream buffers must exist before termination");
        f->armed = true;
        if (f->mode >= 2) {
            if (f->mode != 3) {
                f->backend->cancel(f->backend);
                check(!f->ended, "cancel must wait for the active parser callback to unwind");
                check(tny_parser_test_live_allocations() == f->before,
                      "active parser views must remain alive until the callback returns");
            }
            return;
        }
        check(setenv("TNY_TEST_ALLOC_SCOPE", "backend-parser", 1) == 0, "set scope");
        check(setenv("TNY_TEST_ALLOC_FAIL_AT", "1", 1) == 0, "set fault index");
        tny_alloc_scope_begin("backend-parser");
        f->armed = true;
    }
    if (ev->kind == TNY_EV_ERROR) {
        check(ev->error_code == TNY_EVENT_ERROR_OOM, "terminal error is OOM");
        f->errors++;
    }
    if (ev->kind == TNY_EV_TURN_END) {
        check(ev->stop == (f->mode >= 2 ? TNY_STOP_INTERRUPTED : TNY_STOP_ERROR),
              "terminal stop matches cancellation or OOM");
        f->ends++;
        f->ended = true;
        f->at_failure = tny_parser_test_live_allocations();
        check(f->at_failure == f->baseline,
              "parser allocations must be released before terminal delivery");
    }
}
int main(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    for (int mode = 0; mode < 7; mode++) {
        fixture f = {.mode = mode};
        size_t baseline = tny_parser_test_live_allocations();
        f.baseline = baseline;
        char root[768];
        snprintf(root, sizeof root, "%s/tny-parser-oom-XXXXXX", tmp);
        check(mkdtemp(root) != NULL, "temporary workspace");
        tny_ctx *ctx = tny_ctx_new_explicit(root, root);
        check(ctx != NULL, "context");
        if (mode >= 5) {
            ctx->perm_mode = TNY_MODE_ASK;
            ctx->library_mode = false; /* native terminal tool permission path */
        }
        char error[256];
        http_server *server = http_server_start("parser-test", respond, &f, error, sizeof error);
        check(server != NULL, "server");
        free(ctx->base_url);
        ctx->base_url = xstrdup(http_server_url(server));
        free(ctx->api_key);
        ctx->api_key = xstrdup("parser-test");
        free(ctx->wire_api);
        ctx->wire_api = xstrdup("chat");
        tny_session_state *session = session_new(ctx);
        perm_engine *perm = perm_new(ctx);
        tny_backend *backend = tny_backend_openai_new(ctx);
        check(session && perm && backend, "backend resources");
        f.backend = backend;
        tny_backend_openai_bind(backend, session, perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL);
        check(backend->connect(backend, error, sizeof error) == 0, "connect");
        check(backend->create_or_resume(backend, NULL, error, sizeof error) == 0, "create");
        if (mode >= 5) {
            buf_appends(&f.wire, mode == 5 ? "data: {\"choices\":[{\"delta\":{"
                                           : "{\"choices\":[{\"message\":{");
            buf_appends(
                &f.wire,
                "\"tool_calls\":[{\"index\":0,\"id\":\"retained-call-long-enough-to-allocate\","
                "\"type\":\"function\",\"function\":{\"name\":\"terminal\","
                "\"arguments\":\"{\\\"command\\\":\\\"printf unused\\\"");
            for (int i = 0; i < 4096; i++) buf_appends(&f.wire, " ");
            buf_appends(&f.wire,
                        "}\"}},{\"index\":1,\"id\":\"second-call-long-enough-to-allocate\","
                        "\"type\":\"function\",\"function\":{\"name\":\"terminal\","
                        "\"arguments\":\"{\\\"command\\\":\\\"printf unused\\\"}\"}}]},"
                        "\"finish_reason\":\"tool_calls\"}]}");
            if (mode == 5) buf_appends(&f.wire, "\n\ndata: [DONE]\n\n");
        } else {
            buf_appends(
                &f.wire,
                "data: "
                "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"retained-call-long-enough-"
                "to-allocate\",\"function\":{\"name\":\"list_files\",\"arguments\":\"{");
            for (int i = 0; i < 4096; i++) buf_appends(&f.wire, " ");
            buf_appends(
                &f.wire,
                "}\"}}]}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"arm\"}}]}\n\n");
            if (mode == 4) f.wire.len -= 2; /* final text event dispatches from sse_flush */
            else if (mode == 1)
                buf_appends(&f.wire,
                            "data: {\"choices\":[{\"delta\":{\"content\":\"after\"}}]}\n\n");
            else {
                buf_appends(&f.wire, "data: ");
                for (int i = 0; i < 32768; i++) buf_appends(&f.wire, "x");
            }
        }
        check(backend->send(backend, "OOM retention", NULL, event, &f, error, sizeof error) == 0,
              "send");
        int64_t deadline = monotonic_ms() + 10000;
        while (!f.ended && monotonic_ms() < deadline) {
            struct pollfd fds[HTTP_SERVER_POLLFD_CAPACITY + TNY_BACKEND_POLLFD_MAX];
            int sn = http_server_pollfds(server, fds, HTTP_SERVER_POLLFD_CAPACITY);
            int bn = backend->pollfds(backend, fds + sn, TNY_BACKEND_POLLFD_MAX);
            tny_poll(fds, (nfds_t)(sn + bn), 10);
            http_server_dispatch(server, fds, sn);
            if (backend->dispatch(backend, fds + sn, bn) < 0) break;
            if (mode == 3 && f.armed && !f.ended) backend->cancel(backend);
            if (mode >= 5 && f.parked && !f.ended) {
                struct pollfd parked_fds[TNY_BACKEND_POLLFD_MAX];
                check(backend->pollfds(backend, parked_fds, TNY_BACKEND_POLLFD_MAX) == 0,
                      "permission batch is parked before cancellation");
                backend->cancel(backend);
                check(f.ended && f.cancelled_tools == 2,
                      "parked batch cancellation settles both retained calls");
            }
        }
        check(f.armed && (mode >= 2 || tny_alloc_test_scope_injected()),
              "stream termination exercised");
        check(f.ended && f.errors == (mode >= 2 ? 0 : 1) && f.ends == 1,
              "one terminal event with only the expected error");
        /* Deliberately before destroy, reset, or another turn. */
        check(f.at_failure == baseline && tny_parser_test_live_allocations() == baseline,
              "termination must release SSE and abandoned call allocations immediately");
        unsetenv("TNY_TEST_ALLOC_SCOPE");
        unsetenv("TNY_TEST_ALLOC_FAIL_AT");
        tny_alloc_scope_begin("disabled");
        backend->destroy(backend);
        check(tny_parser_test_live_allocations() == baseline,
              "destruction after cancellation does not leak or double release");
        perm_free(perm);
        session_close(session);
        tny_ctx_free(ctx);
        http_server_destroy(&server);
        buf_free(&f.wire);
        printf("backend parser %s: live allocations %zu -> %zu before teardown\n",
               (const char *[]){"OOM SSE", "OOM JSON", "cancel feed", "cancel outside",
                                "cancel flush", "cancel parked SSE", "cancel parked JSON"}[mode],
               f.before, f.at_failure);
    }
    return 0;
}
