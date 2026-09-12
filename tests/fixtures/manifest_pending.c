/* Drive the real native pending-permission move, not the synchronous prompt
 * hook. Python changes files after "permission", before "allow" or "cancel".
 * Two turns share one permission engine; no ALLOW_ALWAYS is ever sent. */
#include "backends/openai/openai.h"
#include "core/config.h"
#include "core/image_manifest.h"
#include "core/image_service.h"
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only image_service.c is compiled with these link aliases. Track every owned
 * setting, resolved path and loaded source record across failed preparation.
 * The fault matches a lineage string, not a global allocation ordinal. */
static const char *fail_lineage;
static bool fail_destination;
static size_t destination_owned;
static size_t injected, live, acquired, disposed;
static void *owned[128];

static void *track(void *p) {
    if (!p) return NULL;
    for (size_t i = 0; i < sizeof owned / sizeof *owned; i++) {
        if (owned[i]) continue;
        owned[i] = p;
        live++;
        acquired++;
        return p;
    }
    abort();
}

static void untrack(void *p) {
    if (!p) return;
    for (size_t i = 0; i < sizeof owned / sizeof *owned; i++) {
        if (owned[i] != p) continue;
        owned[i] = NULL;
        live--;
        disposed++;
        return;
    }
}

char *manifest_test_strdup(const char *s) {
    if (fail_lineage && strcmp(s, fail_lineage) == 0) {
        injected++;
        return NULL;
    }
    return track(xstrdup(s));
}

void manifest_test_free(void *p) {
    untrack(p);
    free(p);
}

tny_image_manifest *manifest_test_load(const char *path, char *err, size_t len) {
    return track(tny_image_manifest_load(path, err, len));
}

char *manifest_test_resolve(const tny_image_manifest *m, const char *path) {
    /* A replay's preparation resolves reference paths, not artifact_path.
     * Pointer identity targets only its later destination-protection lookup. */
    if (fail_destination && path == m->artifact_path) {
        destination_owned = live;
        injected++;
        return NULL;
    }
    return track(tny_image_manifest_resolve(m, path));
}

void manifest_test_manifest_free(tny_image_manifest *m) {
    untrack(m);
    tny_image_manifest_free(m);
}

static char *permission;
static bool ended;
static int stop_reason;

static void event(const tny_backend_event *ev, void *ud) {
    (void)ud;
    if (ev->kind == TNY_EV_PERMISSION) {
        if (permission || !(ev->perm_options & TNY_PERM_ALLOW_ONCE)) abort();
        permission = xstrdup(ev->perm_id);
        buf_t line;
        buf_init(&line);
        buf_appends(&line, "{\"event\":\"permission\",\"summary\":");
        jescape(&line, ev->perm_summary);
        buf_appends(&line, "}");
        puts(line.data);
        buf_free(&line);
        fflush(stdout);
    } else if (ev->kind == TNY_EV_ERROR) {
        fprintf(stderr, "backend error: %.*s\n", (int)ev->text_len, ev->text);
    } else if (ev->kind == TNY_EV_TOOL_END) {
        fprintf(stderr, "tool result: %s\n", ev->tool_detail ? ev->tool_detail : "");
    } else if (ev->kind == TNY_EV_TURN_END) {
        ended = true;
        stop_reason = ev->stop;
    }
}

static void read_action(char *action, size_t cap) {
    if (!fgets(action, (int)cap, stdin)) abort();
    action[strcspn(action, "\n")] = 0;
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6 && argc != 7) return 2;
    bool destination = argc == 6 && strcmp(argv[5], "destination") == 0;
    if (argc == 6 && !destination) fail_lineage = argv[5];
    tny_ctx ctx = {.cwd = argv[1],
                   .tny_dir = argv[1],
                   .base_url = argv[2],
                   .codex_base_url = argv[3],
                   .tool_profile =
                       strcmp(argv[4], "terminal") == 0 ? TNY_TOOLS_TERMINAL : TNY_TOOLS_ALL,
                   .provider_name = "openai",
                   .model = "fixture-model",
                   .wire_api = "chat",
                   .api_key = "fixture-chat-key",
                   .chatgpt_token = "fixture-image-token",
                   .chatgpt_account_id = "fixture-image-account",
                   .perm_mode = TNY_MODE_ASK,
                   .max_tool_result_bytes = 32768,
                   .no_save = true};
    if (argc == 7) {
        /* Ordinary service entry point: prepare locally, then fail the same
         * execution lookup. No prepared-plan substitute is used here. */
        tny_image_request request = {
            .replay = true, .from_manifest = argv[5], .output_file = argv[6]};
        tny_image_result result;
        char err[512];
        fail_destination = true;
        int rc = tny_image_run(&ctx, &request, &result, err, sizeof err);
        fprintf(stderr, "%s\n", err);
        if (rc != 1 || injected != 1 || destination_owned < 8 || live || acquired != disposed ||
            result.committed || result.operation_id[0] || result.manifest_path[0])
            abort();
        printf("{\"event\":\"ordinary\",\"rc\":%d,\"injected\":%zu,\"live\":%zu,"
               "\"acquired\":%zu,\"disposed\":%zu,\"destination_owned\":%zu}\n",
               rc, injected, live, acquired, disposed, destination_owned);
        return 0;
    }
    perm_engine *perm = perm_new(&ctx);
    if (!perm) abort();
    for (int round = 0; round < 2; round++) {
        tny_session_state *session = session_new(&ctx);
        tny_backend *backend = tny_backend_openai_new(&ctx);
        if (!session || !backend) abort();
        tny_backend_openai_bind(backend, session, perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL);
        char err[512], action[32];
        ended = false;
        stop_reason = -1;
        int questions = 0;
        bool destroyed = false;
        if (backend->connect(backend, err, sizeof err) ||
            backend->create_or_resume(backend, NULL, err, sizeof err) ||
            backend->send(backend, "fixture image", NULL, event, NULL, err, sizeof err)) {
            fprintf(stderr, "backend failed: %s\n", err);
            abort();
        }
        while (!ended) {
            if (permission) {
                questions++;
                if (questions != 1 || perm_grant_count(perm) != 0) abort();
                read_action(action, sizeof action);
                if (strcmp(action, "allow") == 0) {
                    /* Arm only after actual preparation and pending transfer. */
                    fail_destination = destination && round == 0;
                    backend->respond_permission(backend, permission, TNY_PERM_DECISION_ALLOW);
                } else if (strcmp(action, "cancel") == 0) backend->cancel(backend);
                else if (strcmp(action, "destroy") == 0) {
                    backend->destroy(backend);
                    destroyed = ended = true;
                } else abort();
                free(permission);
                permission = NULL;
                continue;
            }
            struct pollfd fds[TNY_BACKEND_POLLFD_MAX];
            int n = backend->pollfds(backend, fds, TNY_BACKEND_POLLFD_MAX);
            if (tny_poll(fds, (nfds_t)n, 10) < 0) abort();
            if (backend->dispatch(backend, fds, n) < 0 && !ended) abort();
        }
        bool allocation_failure = fail_lineage && round == 0;
        if (questions != (allocation_failure ? 0 : 1) || perm_grant_count(perm) != 0) abort();
        /* Failed prepare must release everything before backend destruction,
         * not leave an approved or pending plan for teardown to rescue. */
        if (allocation_failure && (injected != 1 || live || acquired < 3 || acquired != disposed))
            abort();
        /* Execution failure must release the transferred plan before teardown. */
        if (destination && round == 0 &&
            (injected != 1 || destination_owned < 8 || live || acquired != disposed))
            abort();
        if (!destroyed) backend->destroy(backend);
        session_close(session);
        if (live) abort();
        printf("{\"event\":\"ended\",\"round\":%d,\"grants\":%d,\"stop\":%d,"
               "\"questions\":%d,\"injected\":%zu,\"live\":%zu,\"acquired\":%zu,\"disposed\":%zu,"
               "\"destination_owned\":%zu}\n",
               round, perm_grant_count(perm), stop_reason, questions, injected, live, acquired,
               disposed, destination_owned);
        fail_lineage = NULL;
        fail_destination = false;
        fflush(stdout);
        if (round == 0) {
            read_action(action, sizeof action);
            if (strcmp(action, "next") != 0) abort();
        }
    }
    perm_free(perm);
    return 0;
}
