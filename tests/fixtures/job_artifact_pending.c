/* Actual native pending permissions. Python mutates job/manifest/file state
 * after the permission event, then grants once. Two turns share one engine. */
#include "backends/openai/openai.h"
#include "core/config.h"
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *permission;
static bool ended;

static void event(const tny_backend_event *ev, void *ud) {
    (void)ud;
    if (ev->kind == TNY_EV_PERMISSION) {
        if (permission || !(ev->perm_options & TNY_PERM_ALLOW_ONCE)) abort();
        permission = xstrdup(ev->perm_id);
        buf_t out;
        buf_init(&out);
        buf_appends(&out, "{\"event\":\"permission\",\"summary\":");
        jescape(&out, ev->perm_summary);
        buf_appends(&out, "}");
        puts(out.data);
        buf_free(&out);
        fflush(stdout);
    } else if (ev->kind == TNY_EV_ERROR) {
        fprintf(stderr, "backend error: %.*s\n", (int)ev->text_len, ev->text);
    } else if (ev->kind == TNY_EV_TURN_END) ended = true;
}

static void action(const char *expected) {
    char line[32];
    if (!fgets(line, sizeof line, stdin)) abort();
    line[strcspn(line, "\n")] = 0;
    if (strcmp(line, expected) != 0) abort();
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    char *state = path_tny_dir();
    tny_ctx ctx = {.cwd = argv[1],
                   .tny_dir = state,
                   .base_url = argv[2],
                   .codex_base_url = argv[3],
                   .tool_profile = TNY_TOOLS_ALL,
                   .provider_name = "openai",
                   .model = "fixture-model",
                   .wire_api = argv[4],
                   .api_key = "fixture-chat-key",
                   .chatgpt_token = "fixture-image-token",
                   .chatgpt_account_id = "fixture-image-account",
                   .perm_mode = TNY_MODE_ASK,
                   .image_input = TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED,
                   .max_tool_result_bytes = 32768,
                   .no_save = true};
    perm_engine *perm = perm_new(&ctx);
    if (!state || !perm) abort();
    for (int round = 0; round < 2; round++) {
        tny_session_state *session = session_new(&ctx);
        tny_backend *backend = tny_backend_openai_new(&ctx);
        if (!session || !backend) abort();
        tny_backend_openai_bind(backend, session, perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL);
        char err[512];
        ended = false;
        int questions = 0;
        if (backend->connect(backend, err, sizeof err) ||
            backend->create_or_resume(backend, NULL, err, sizeof err) ||
            backend->send(backend, "fixture image", NULL, event, NULL, err, sizeof err)) {
            fprintf(stderr, "backend failed: %s\n", err);
            abort();
        }
        while (!ended) {
            if (permission) {
                questions++;
                action("allow");
                backend->respond_permission(backend, permission, TNY_PERM_DECISION_ALLOW);
                free(permission);
                permission = NULL;
                continue;
            }
            struct pollfd fds[TNY_BACKEND_POLLFD_MAX];
            int n = backend->pollfds(backend, fds, TNY_BACKEND_POLLFD_MAX);
            if (tny_poll(fds, (nfds_t)n, 10) < 0) abort();
            if (backend->dispatch(backend, fds, n) < 0 && !ended) abort();
        }
        backend->destroy(backend);
        session_close(session);
        printf("{\"event\":\"ended\",\"round\":%d,\"grants\":%d,\"questions\":%d}\n", round,
               perm_grant_count(perm), questions);
        fflush(stdout);
        if (round == 0) action("next");
    }
    perm_free(perm);
    free(state);
    return 0;
}
