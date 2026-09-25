/* Run real provider/runtime regressions against fully injected library objects. */
#include "greatest.h"
/* A whole active turn has one allocator scope, including every dispatch,
 * request retry/tool continuation and runtime finalization. The mock process
 * owns its allocations; only library/provider allocations enter the counter. */
#include "core/config.h"
#include "core/tools.h"
#include "core/perm.h"
#include "core/runtime.h"
#include "core/execution.h"
#include "util/execution_command.h"
#include "core/session.h"
#include "util/alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int drain_turn(tny_engine *engine, bool injected_turn) {
    int errors = 0, ends = 0;
    char err[512];
    int64_t deadline = monotonic_ms() + 10000;
    while (monotonic_ms() < deadline) {
        bool settled = injected_turn && tny_alloc_test_settlement_count() > 0;
        size_t before = tny_alloc_test_scope_count();
        tny_owned_event *event = NULL;
        tny_engine_next rc = tny_engine_next_event(engine, 20, &event, err, sizeof err);
        if (settled && before != tny_alloc_test_scope_count()) {
            tny_owned_event_free(event);
            return 29;
        }
        if (rc == TNY_ENGINE_NEXT_DRAINED) break;
        if (rc == TNY_ENGINE_NEXT_TIMEOUT) continue;
        if (rc != TNY_ENGINE_NEXT_EVENT || !event) return 20;
        bool oom = injected_turn && tny_alloc_test_scope_injected();
        if (event->ev.kind == TNY_EV_PERMISSION) {
            tny_engine_respond_permission(engine, event->ev.perm_id, TNY_PERM_DECISION_ALLOW);
        }
        if (event->ev.kind == TNY_EV_ERROR) {
            if (!oom || event->ev.error_code != TNY_EVENT_ERROR_OOM || ends) {
                fprintf(stderr, "unexpected error: injected_turn=%d oom=%d code=%d text=%.*s\n",
                        injected_turn, oom, event->ev.error_code, (int)event->ev.text_len,
                        event->ev.text ? event->ev.text : "");
                tny_owned_event_free(event);
                return 22;
            }
            errors++;
        }
        if (event->ev.kind == TNY_EV_TURN_END) {
            if (event->ev.stop != (oom ? TNY_STOP_ERROR : TNY_STOP_DONE)) return 23;
            ends++;
        }
        tny_owned_event_free(event);
    }
    bool oom = injected_turn && tny_alloc_test_scope_injected();
    return ends == 1 && errors == (oom ? 1 : 0) ? 0 : 24;
}

static int turn_sweep_case(const char *provider, const char *index, const char *url,
                           const char *root, const char *report) {
    tny_alloc_scope_begin("disabled");
    tny_ctx *ctx = tny_ctx_new_explicit(root, root);
    if (!ctx) return 10;
    ctx->library_mode = true;
    ctx->no_save = false;
    ctx->mcp_disabled = true;
    ctx->perm_mode = TNY_MODE_YOLO;
    free(ctx->model);
    ctx->model = NULL;
    free(ctx->api_key);
    ctx->api_key = xstrdup("fault-test-key");
    free(ctx->base_url);
    ctx->base_url = xstrdup(url);
    free(ctx->wire_api);
    ctx->wire_api = xstrdup(strcmp(provider, "openai-chat") == 0 ? "chat" : "responses");
    ctx->backend = TNY_BK_OPENAI;
    tny_session_state *session = session_new(ctx);
    perm_engine *perm = perm_new(ctx);
    tny_engine *engine = tny_engine_new(ctx, session, perm, NULL, NULL);
    tny_backend *backend = tny_backend_openai_new(ctx);
    char err[512];
    if (!engine || !backend ||
        tny_engine_prepare(engine, backend, TNY_ENGINE_PREPARE_FRESH, err, sizeof err) != 0) {
        FILE *failure = fopen(report, "w");
        if (failure) {
            fprintf(failure, "prepare: %s\n", err);
            fclose(failure);
        }
        return 12;
    }
    /* Start/admission constructors have their own exhaustive public API sweep.
     * This scope begins with a successfully admitted active turn. */
    if (tny_engine_start(engine, "representative fault turn", NULL, err, sizeof err) != 0)
        return 13;
    setenv("TNY_TEST_ALLOC_SCOPE", "provider-turn", 1);
    setenv("TNY_TEST_ALLOC_FAIL_AT", index, 1);
    tny_alloc_scope_begin("provider-turn");
    int rc = drain_turn(engine, true);
    size_t count = tny_alloc_test_scope_count();
    bool injected = tny_alloc_test_scope_injected();
    size_t settlements = tny_alloc_test_settlement_count();
    size_t after_failure = tny_alloc_test_settlement_allocations();
    unsetenv("TNY_TEST_ALLOC_SCOPE");
    unsetenv("TNY_TEST_ALLOC_FAIL_AT");
    if (!rc && after_failure) rc = 25;
    if (!rc && injected && settlements == 0) rc = 26;
    tny_alloc_scope_begin("disabled");
    if (!rc && tny_engine_start(engine, "successful recovery turn", NULL, err, sizeof err) != 0)
        rc = 27;
    if (!rc) rc = drain_turn(engine, false);
    tny_engine_free(engine);
    perm_free(perm);
    session_close(session);
    tny_ctx_free(ctx);
    FILE *out = fopen(report, "w");
    if (!out) return 28;
    fprintf(out, "%zu %d %zu %zu %d\n", count, injected, settlements, after_failure, rc);
    fclose(out);
    return rc;
}

SUITE_EXTERN(openai_suite);
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    /* Native lifecycle suites execute tools through this same trusted test
     * image, just like tests/test_main.c; never fall back to in-process tools. */
    if (argc == 2 && strcmp(argv[1], "--exec-command") == 0) return tny_exec_command_main();
    if (argc == 2 && strcmp(argv[1], "--exec-server") == 0) return tny_execution_server_main();
    if (argc == 2 && strcmp(argv[1], "--native-storage-guard") == 0) {
        tools_call call = {0};
        /* The guard must reject authority before dereferencing or releasing it. */
        call.custom_call = (struct custom_tool_pending *)&call;
        tools_call_release_storage(&call);
        return 0; /* reachable only in a private guard-removed mutant */
    }
    if (argc == 7 && strcmp(argv[1], "--turn-sweep") == 0)
        return turn_sweep_case(argv[2], argv[3], argv[4], argv[5], argv[6]);
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(openai_suite);
    GREATEST_MAIN_END();
}
