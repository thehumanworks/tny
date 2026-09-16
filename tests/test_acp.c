/* test_acp.c — ACP client transport seams (docs/adr/0017): agent-URL
 * detection, JSON-RPC message builders, and pollfd assembly. Pure logic
 * only; lifecycle and wire behavior live in tests/integration/test_acp*.sh. */
#include "greatest.h"
#include "backends/acp/acp_client.h"
#include "util/alloc.h"
#include "core/runtime.h"
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    tny_backend_event event;
    int count;
    char tool_name[128];
    char tool_id[128];
    char tool_detail[512];
} acp_event_capture;

static void capture_event(const tny_backend_event *ev, void *ud) {
    acp_event_capture *capture = ud;
    capture->event = *ev;
    if (ev->tool_name) {
        snprintf(capture->tool_name, sizeof capture->tool_name, "%s", ev->tool_name);
        capture->event.tool_name = capture->tool_name;
    }
    if (ev->tool_id) {
        snprintf(capture->tool_id, sizeof capture->tool_id, "%s", ev->tool_id);
        capture->event.tool_id = capture->tool_id;
    }
    if (ev->tool_detail) {
        snprintf(capture->tool_detail, sizeof capture->tool_detail, "%s", ev->tool_detail);
        capture->event.tool_detail = capture->tool_detail;
    }
    capture->count++;
}

TEST agent_is_ws_detects_only_ws_urls(void) {
    ASSERT(ac_agent_is_ws("ws://127.0.0.1:9100"));
    ASSERT(ac_agent_is_ws("wss://agents.example/acp"));
    ASSERT_FALSE(ac_agent_is_ws(NULL));
    ASSERT_FALSE(ac_agent_is_ws(""));
    ASSERT_FALSE(ac_agent_is_ws("gemini"));
    ASSERT_FALSE(ac_agent_is_ws("/usr/bin/agent"));
    ASSERT_FALSE(ac_agent_is_ws("wsx://not-a-socket"));
    ASSERT_FALSE(ac_agent_is_ws("unix:///tmp/agent.sock"));
    PASS();
}

TEST fmt_builders_produce_exact_json(void) {
    buf_t b;
    buf_init(&b);
    acp_fmt_request(&b, 7, "session/prompt", "{\"x\":1}");
    ASSERT_STR_EQ(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"session/prompt\",\"params\":{\"x\":1}}",
        b.data);
    buf_clear(&b);
    acp_fmt_request(&b, 1, "initialize", NULL);
    ASSERT_STR_EQ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}", b.data);
    buf_clear(&b);
    acp_fmt_notify(&b, "session/cancel", "{\"sessionId\":\"s\"}");
    ASSERT_STR_EQ(
        "{\"jsonrpc\":\"2.0\",\"method\":\"session/cancel\",\"params\":{\"sessionId\":\"s\"}}",
        b.data);
    buf_clear(&b);
    acp_fmt_result(&b, "9", "{\"ok\":true}");
    ASSERT_STR_EQ("{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"ok\":true}}", b.data);
    buf_clear(&b);
    acp_fmt_result(&b, NULL, NULL);
    ASSERT_STR_EQ("{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":null}", b.data);
    buf_free(&b);
    PASS();
}

/* The stdio pollfd set: stdout + stderr of the spawned agent, POLLIN only,
 * revents cleared. fd 0 is a valid descriptor and must not be dropped. */
TEST transport_pollfds_stdio(void) {
    ac_impl o;
    memset(&o, 0, sizeof o);
    o.in_fd = 3;
    o.out_fd = 0; /* boundary: fd 0 is real */
    o.err_fd = 7;
    struct pollfd fds[8];
    memset(fds, 0x5a, sizeof fds);
    int n = ac_transport_pollfds(&o, fds, 8);
    ASSERT_EQ(2, n);
    ASSERT_EQ(0, fds[0].fd);
    ASSERT_EQ(POLLIN, fds[0].events);
    ASSERT_EQ(0, fds[0].revents);
    ASSERT_EQ(7, fds[1].fd);
    ASSERT_EQ(POLLIN, fds[1].events);
    ASSERT_EQ(0, fds[1].revents);
    PASS();
}

TEST transport_pollfds_respects_max_and_missing_fds(void) {
    ac_impl o;
    memset(&o, 0, sizeof o);
    o.out_fd = 5;
    o.err_fd = 7;
    struct pollfd fds[8];

    /* max smaller than the candidate set: exactly max entries written */
    memset(fds, 0x5a, sizeof fds);
    ASSERT_EQ(1, ac_transport_pollfds(&o, fds, 1));
    ASSERT_EQ(5, fds[0].fd);
    ASSERT_EQ(0x5a5a5a5a, fds[1].fd); /* untouched beyond max */

    ASSERT_EQ(0, ac_transport_pollfds(&o, fds, 0));

    /* closed fds are skipped entirely */
    o.out_fd = -1;
    ASSERT_EQ(1, ac_transport_pollfds(&o, fds, 8));
    ASSERT_EQ(7, fds[0].fd);
    o.err_fd = -1;
    ASSERT_EQ(0, ac_transport_pollfds(&o, fds, 8));
    PASS();
}

TEST usage_update_preserves_context_accounting(void) {
    const char *json = "{\"update\":{\"sessionUpdate\":\"usage_update\","
                       "\"used\":321,\"size\":4096,\"cost\":0.125}}";
    yyjson_doc *doc = jparse(json, strlen(json));
    ASSERT(doc);
    acp_event_capture capture = {0};
    ac_impl o;
    memset(&o, 0, sizeof o);
    o.cb = capture_event;
    o.ud = &capture;
    ac_handle_update(&o, yyjson_doc_get_root(doc));
    ASSERT_EQ(1, capture.count);
    ASSERT_EQ(TNY_EV_USAGE, capture.event.kind);
    ASSERT_EQ(321, capture.event.context_used);
    ASSERT_EQ(4096, capture.event.context_size);
    ASSERT(capture.event.has_cost);
    ASSERT(capture.event.cost > 0.124 && capture.event.cost < 0.126);
    yyjson_doc_free(doc);
    PASS();
}

TEST sparse_tool_update_emits_progress(void) {
    const char *json = "{\"update\":{\"sessionUpdate\":\"tool_call_update\","
                       "\"toolCallId\":\"call-7\",\"title\":\"shell\","
                       "\"status\":\"in_progress\",\"content\":[{"
                       "\"type\":\"content\",\"content\":{"
                       "\"type\":\"text\",\"text\":\"halfway\"}}]}}";
    yyjson_doc *doc = jparse(json, strlen(json));
    ASSERT(doc);
    acp_event_capture capture = {0};
    ac_impl o;
    memset(&o, 0, sizeof o);
    o.cb = capture_event;
    o.ud = &capture;
    ac_handle_update(&o, yyjson_doc_get_root(doc));
    ASSERT_EQ(1, capture.count);
    ASSERT_EQ(TNY_EV_TOOL_PROGRESS, capture.event.kind);
    ASSERT_STR_EQ("call-7", capture.event.tool_id);
    ASSERT_STR_EQ("shell", capture.event.tool_name);
    ASSERT_STR_EQ("halfway", capture.event.tool_detail);
    yyjson_doc_free(doc);
    PASS();
}

TEST emergency_cancel_releases_pending_protocol_state(void) {
    tny_ctx ctx = {0};
    tny_backend *backend = tny_backend_acp_new(&ctx);
    ASSERT(backend);
    ac_impl *o = backend->impl;
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    o->in_fd = fds[1];
    o->out_fd = fds[0];
    o->session_id = xstrdup("session-emergency");
    o->turn_active = true;
    o->nperms = 1;
    o->perms[0].id_raw = xstrdup("42");
    o->perms[0].summary = xstrdup("pending permission");
    acp_reader_feed(&o->out_r, "partial", 7);
    o->out_r.overflow = true;
    tny_alloc_settlement_begin();
    backend->cancel(backend);
    tny_alloc_settlement_end();
    ASSERT(o->cancelled);
    ASSERT_FALSE(o->turn_active);
    ASSERT_EQ(0, o->nperms);
    ASSERT_FALSE(o->out_r.overflow);
    ASSERT_EQ(0, o->out_r.buf.len);
    ASSERT_STR_EQ("session-emergency", o->session_id);
    ASSERT_EQ(-1, fcntl(fds[0], F_GETFD));
    ASSERT_EQ(EBADF, errno);
    ASSERT_EQ(-1, fcntl(fds[1], F_GETFD));
    ASSERT_EQ(EBADF, errno);
    tny_alloc_settlement_begin();
    backend->cancel(backend);
    tny_alloc_settlement_end();
    backend->destroy(backend);
    PASS();
}

TEST emergency_cancel_reaps_sigterm_ignoring_child_and_retries(void) {
    char root[] = "/tmp/tny-acp-oom-XXXXXX";
    ASSERT(mkdtemp(root));
    tny_ctx *ctx = tny_ctx_new_explicit(root, root);
    ASSERT(ctx);
    ctx->backend = TNY_BK_ACP;
    ctx->no_save = true;
    free(ctx->model);
    ctx->model = NULL;
    char agent[4096];
    ASSERT(realpath("tests/integration/fake_acp_agent.py", agent));
    char *argv[] = {"python3", agent, "--oom-settlement", NULL};
    ctx->agent_argv = argv;
    tny_session_state *session = session_new(ctx);
    perm_engine *perm = perm_new(ctx);
    tny_engine *engine = tny_engine_new(ctx, session, perm, NULL, NULL);
    tny_backend *backend = tny_backend_acp_new(ctx);
    ASSERT(engine && backend);
    ac_impl *o = backend->impl;
    char err[256];
    ASSERT_EQ(0, tny_engine_prepare(engine, backend, TNY_ENGINE_PREPARE_FRESH, err, sizeof err));
    ASSERT_EQ(0, tny_engine_start(engine, "park", NULL, err, sizeof err));
    tny_owned_event *event = NULL;
    bool ready = false;
    for (int i = 0; i < 100 && !ready; i++) {
        tny_engine_next rc = tny_engine_next_event(engine, 50, &event, err, sizeof err);
        if (rc == TNY_ENGINE_NEXT_EVENT) {
            ready = event->ev.kind == TNY_EV_TEXT_DELTA;
            tny_owned_event_free(event);
        }
    }
    ASSERT(ready);
    pid_t child = o->pid;
    ASSERT(child > 0);
    tny_alloc_scope_begin("acp-emergency");
    int64_t start = monotonic_ms();
    tny_engine_fail_oom(engine);
    ASSERT(monotonic_ms() - start < 2000);
#ifdef TNY_ALLOC_TESTING
    ASSERT_EQ(0, tny_alloc_test_settlement_allocations());
#endif
    ASSERT_EQ(0, o->pid);
    ASSERT_EQ(-1, waitpid(child, NULL, WNOHANG));
    ASSERT_EQ(ECHILD, errno);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(engine, 0, &event, err, sizeof err));
    ASSERT_EQ(TNY_EV_ERROR, event->ev.kind);
    ASSERT_EQ(TNY_EVENT_ERROR_OOM, event->ev.error_code);
    tny_owned_event_free(event);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(engine, 0, &event, err, sizeof err));
    ASSERT_EQ(TNY_EV_TURN_END, event->ev.kind);
    ASSERT_EQ(TNY_STOP_ERROR, event->ev.stop);
    tny_owned_event_free(event);
    ASSERT_EQ(TNY_ENGINE_NEXT_DRAINED, tny_engine_next_event(engine, 0, &event, err, sizeof err));
    tny_alloc_scope_begin("disabled");
    ASSERT_EQ(0, tny_engine_start(engine, "retry", NULL, err, sizeof err));
    int terminals = 0;
    for (int i = 0; i < 100; i++) {
        tny_engine_next rc = tny_engine_next_event(engine, 50, &event, err, sizeof err);
        if (rc == TNY_ENGINE_NEXT_DRAINED) break;
        if (rc == TNY_ENGINE_NEXT_TIMEOUT) continue;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, rc);
        ASSERT(event->ev.kind != TNY_EV_ERROR);
        if (event->ev.kind == TNY_EV_TURN_END) {
            ASSERT_EQ(TNY_STOP_DONE, event->ev.stop);
            terminals++;
        }
        tny_owned_event_free(event);
    }
    ASSERT_EQ(1, terminals);
    tny_engine_free(engine);
    perm_free(perm);
    session_close(session);
    ctx->agent_argv = NULL;
    tny_ctx_free(ctx);
    PASS();
}

#ifdef TNY_ALLOC_TESTING
TEST message_oom_stops_multiline_batch_and_event_copy(void) {
    static const char update[] =
        "{\"method\":\"session/update\",\"params\":{\"update\":{"
        "\"sessionUpdate\":\"agent_message_chunk\",\"content\":{"
        "\"type\":\"text\",\"text\":\"owned text long enough to allocate a retained payload\"}}}}";
    for (int batch = 0; batch < 5; batch++) {
        size_t allocations = 0;
        for (size_t fault = 0; fault <= allocations; fault++) {
            tny_alloc_scope_begin("disabled");
            char root[] = "/tmp/tny-acp-message-XXXXXX";
            ASSERT(mkdtemp(root));
            tny_ctx *ctx = tny_ctx_new_explicit(root, root);
            ASSERT(ctx);
            ctx->backend = TNY_BK_ACP;
            ctx->no_save = true;
            tny_session_state *session = session_new(ctx);
            perm_engine *perm = perm_new(ctx);
            tny_engine *engine = tny_engine_new(ctx, session, perm, NULL, NULL);
            tny_backend *backend = tny_backend_acp_new(ctx);
            ASSERT(engine && backend);
            ac_impl *o = backend->impl;
            int input[2], output[2];
            ASSERT_EQ(0, pipe(input));
            ASSERT_EQ(0, pipe(output));
            o->in_fd = input[1];
            o->out_fd = output[0];
            ASSERT_EQ(0, fcntl(o->out_fd, F_SETFL, O_NONBLOCK));
            o->session_id = xstrdup("message-fault");
            o->effort_noted = true;
            char err[256];
            ASSERT_EQ(0, tny_engine_prepare(engine, backend, TNY_ENGINE_PREPARE_RESUMED, err,
                                            sizeof err));
            ASSERT_EQ(0, tny_engine_start(engine, "parse", NULL, err, sizeof err));
            char messages[2048];
            snprintf(messages, sizeof messages, batch == 1 ? "[%s,%s]\n" : "malformed\n%s\n%s\n",
                     update, update);
            if (batch >= 3)
                snprintf(
                    messages, sizeof messages,
                    "{\"jsonrpc\":\"2.0\",\"id\":%s,\"method\":\"%s\",\"params\":{}}\n%s\n%s\n",
                    batch == 3 ? "{}" : "true", batch == 3 ? "cursor/task" : "unknown/request",
                    update, update);
            /* Both messages are already buffered before injection. The sweep
             * includes line copy, jparse, batch jwrite/jparse and owned events. */
            if (batch == 2)
                ASSERT_EQ((ssize_t)strlen(messages), write(output[1], messages, strlen(messages)));
            else acp_reader_feed(&o->out_r, messages, strlen(messages));
            char index[32];
            snprintf(index, sizeof index, "%zu", fault);
            ASSERT_EQ(0, setenv("TNY_TEST_ALLOC_SCOPE", "acp-message", 1));
            ASSERT_EQ(0, setenv("TNY_TEST_ALLOC_FAIL_AT", index, 1));
            tny_alloc_scope_begin("acp-message");
            (void)tny_engine_dispatch(engine, NULL, 0);
            if (!fault) allocations = tny_alloc_test_scope_count();
            else {
                ASSERT(tny_alloc_test_scope_injected());
                ASSERT_EQ(fault, tny_alloc_test_scope_count());
                ASSERT_EQ(0, tny_alloc_test_settlement_allocations());
                ASSERT_FALSE(o->turn_active);
            }
            unsetenv("TNY_TEST_ALLOC_SCOPE");
            unsetenv("TNY_TEST_ALLOC_FAIL_AT");
            int texts = 0, errors = 0, ends = 0;
            tny_owned_event *event = NULL;
            for (;;) {
                tny_engine_next rc = tny_engine_next_event(engine, 0, &event, err, sizeof err);
                if (rc == TNY_ENGINE_NEXT_DRAINED || rc == TNY_ENGINE_NEXT_TIMEOUT) break;
                ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, rc);
                if (event->ev.kind == TNY_EV_TEXT_DELTA) texts++;
                if (event->ev.kind == TNY_EV_ERROR) {
                    ASSERT_EQ(0, ends);
                    ASSERT_EQ(TNY_EVENT_ERROR_OOM, event->ev.error_code);
                    errors++;
                }
                if (event->ev.kind == TNY_EV_TURN_END) {
                    ASSERT_EQ(1, errors);
                    ASSERT_EQ(TNY_STOP_ERROR, event->ev.stop);
                    ends++;
                }
                tny_owned_event_free(event);
            }
            ASSERT_EQ(fault ? 1 : 0, errors);
            ASSERT_EQ(fault ? 1 : 0, ends);
            if (!fault) ASSERT_EQ(2, texts);
            tny_alloc_scope_begin("disabled");
            tny_engine_free(engine);
            close(input[0]);
            close(output[1]);
            perm_free(perm);
            session_close(session);
            tny_ctx_free(ctx);
        }
        ASSERT(allocations >= (batch == 1 ? 10u : 6u));
    }
    PASS();
}
#endif

SUITE(acp_suite) {
#ifdef TNY_ALLOC_TESTING
    RUN_TEST(message_oom_stops_multiline_batch_and_event_copy);
#endif
    RUN_TEST(emergency_cancel_reaps_sigterm_ignoring_child_and_retries);
    RUN_TEST(emergency_cancel_releases_pending_protocol_state);
    RUN_TEST(agent_is_ws_detects_only_ws_urls);
    RUN_TEST(fmt_builders_produce_exact_json);
    RUN_TEST(transport_pollfds_stdio);
    RUN_TEST(transport_pollfds_respects_max_and_missing_fds);
    RUN_TEST(usage_update_preserves_context_accounting);
    RUN_TEST(sparse_tool_update_emits_progress);
}
