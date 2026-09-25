/* Drive production code-mode preparation in a fresh execution process.
 * Python mutates files while the owning harness waits for a prompt response.
 * Two turns share one permission engine; no ALLOW_ALWAYS is ever sent. */
#include "backends/openai/openai.h"
#include "core/config.h"
#include "core/execution.h"
#include "util/execution_command.h"
#include "util/process.h"
#include "core/image_manifest.h"
#include "core/image_service.h"
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static int questions;
static bool destroy_requested;
static tny_backend *active_backend;
static perm_engine *active_perm;

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

static tny_perm_decision blocking_prompt(const char *tool, const char *summary, void *ud) {
    (void)tool;
    (void)ud;
    tny_backend_event ev = {.kind = TNY_EV_PERMISSION,
                            .perm_id = "manifest-nested-permission",
                            .perm_summary = summary,
                            .perm_options = TNY_PERM_ALLOW_ONCE | TNY_PERM_DENY};
    event(&ev, NULL);
    ++questions;
    if (questions != 1 || perm_grant_count(active_perm) != 0) abort();
    char action[32];
    read_action(action, sizeof action);
    free(permission);
    permission = NULL;
    if (strcmp(action, "allow") == 0) return TNY_PERM_DECISION_ALLOW;
    if (strcmp(action, "cancel") != 0 && strcmp(action, "destroy") != 0) abort();
    destroy_requested = strcmp(action, "destroy") == 0;
    /* Cancel now, destroy only after dispatch unwinds from this callback. */
    active_backend->cancel(active_backend);
    return TNY_PERM_DECISION_DENY;
}
static void publish_stats(const char *path) {
    buf_t json = {0};
    buf_appendf(&json,
                "{\"injected\":%zu,\"live\":%zu,\"acquired\":%zu,"
                "\"disposed\":%zu,\"destination_owned\":%zu}",
                injected, live, acquired, disposed, destination_owned);
    if (buf_oom(&json) || file_write_atomic(path, json.data, json.len)) abort();
    buf_free(&json);
}
static bool collect_stats(const char *path) {
    yyjson_doc *d = jparse_file(path);
    if (!d) return false;
    yyjson_val *root = yyjson_doc_get_root(d);
    injected = (size_t)jget_int(root, "injected", -1);
    live = (size_t)jget_int(root, "live", -1);
    acquired = (size_t)jget_int(root, "acquired", -1);
    disposed = (size_t)jget_int(root, "disposed", -1);
    destination_owned = (size_t)jget_int(root, "destination_owned", -1);
    yyjson_doc_free(d);
    return true;
}

int main(int argc, char **argv) {
    if (tny_process_scope_admit() != 0) return 2;
    if (argc == 2 && strcmp(argv[1], "--exec-server") == 0) {
        fail_lineage = getenv("TNY_TEST_MANIFEST_LINEAGE");
        fail_destination = getenv("TNY_TEST_MANIFEST_DESTINATION") != NULL;
        int rc = tny_execution_server_main();
        const char *stats = getenv("TNY_TEST_MANIFEST_STATS");
        if (stats) publish_stats(stats);
        return rc;
    }
    if (argc == 2 && strcmp(argv[1], "--exec-command") == 0) return tny_exec_command_main();
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
    char stats_path[] = "/tmp/tny-manifest-stats-XXXXXX";
    int stats_fd = mkstemp(stats_path);
    if (stats_fd < 0 || setenv("TNY_TEST_MANIFEST_STATS", stats_path, 1)) abort();
    close(stats_fd);
    perm_engine *perm = perm_new(&ctx);
    if (!perm) abort();
    active_perm = perm;
    for (int round = 0; round < 2; round++) {
        tny_session_state *session = session_new(&ctx);
        tny_backend *backend = tny_backend_openai_new(&ctx);
        if (!session || !backend) abort();
        active_backend = backend;
        tny_backend_openai_bind(backend, session, perm, blocking_prompt, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL);
        char err[512], action[32];
        ended = false;
        stop_reason = -1;
        questions = 0;
        destroy_requested = false;
        (void)unlink(stats_path);
        if (fail_lineage) {
            if (setenv("TNY_TEST_MANIFEST_LINEAGE", fail_lineage, 1)) abort();
        } else if (unsetenv("TNY_TEST_MANIFEST_LINEAGE")) abort();
        if (destination && round == 0) {
            if (setenv("TNY_TEST_MANIFEST_DESTINATION", "1", 1)) abort();
        } else if (unsetenv("TNY_TEST_MANIFEST_DESTINATION")) abort();
        if (backend->connect(backend, err, sizeof err) ||
            backend->create_or_resume(backend, NULL, err, sizeof err) ||
            backend->send(backend, "fixture image", NULL, event, NULL, err, sizeof err)) {
            fprintf(stderr, "backend failed: %s\n", err);
            abort();
        }
        while (!ended) {
            /* run_code owns a synchronous prompt callback; a second parked
             * permission path would silently bypass the execution server. */
            if (permission) abort();
            struct pollfd fds[TNY_BACKEND_POLLFD_MAX];
            int n = backend->pollfds(backend, fds, TNY_BACKEND_POLLFD_MAX);
            if (tny_poll(fds, (nfds_t)n, 10) < 0) abort();
            if (backend->dispatch(backend, fds, n) < 0 && !ended) abort();
        }
        bool stats_available = collect_stats(stats_path);
        if (!stats_available) injected = live = acquired = disposed = destination_owned = 0;
        bool allocation_failure = fail_lineage && round == 0;
        if (questions != (allocation_failure ? 0 : 1) || perm_grant_count(perm) != 0) abort();
        /* Failed prepare must release everything before backend destruction,
         * not leave an approved or pending plan for teardown to rescue. */
        if (allocation_failure &&
            (!stats_available || injected != 1 || live || acquired < 3 || acquired != disposed))
            abort();
        /* Execution failure must release the transferred plan before teardown. */
        if (destination && round == 0 &&
            (!stats_available || injected != 1 || destination_owned < 8 || live ||
             acquired != disposed))
            abort();
        if (!stats_available && stop_reason != TNY_STOP_INTERRUPTED && !destroy_requested) abort();
        backend->destroy(backend);
        active_backend = NULL;
        session_close(session);
        if (live) abort();
        printf("{\"event\":\"ended\",\"round\":%d,\"grants\":%d,\"stop\":%d,"
               "\"questions\":%d,\"injected\":%zu,\"live\":%zu,\"acquired\":%zu,\"disposed\":%zu,"
               "\"destination_owned\":%zu,\"stats_available\":%s,\"destroy_requested\":%s}\n",
               round, perm_grant_count(perm), stop_reason, questions, injected, live, acquired,
               disposed, destination_owned, stats_available ? "true" : "false",
               destroy_requested ? "true" : "false");
        fail_lineage = NULL;
        fail_destination = false;
        fflush(stdout);
        if (round == 0) {
            read_action(action, sizeof action);
            if (strcmp(action, "next") != 0) abort();
        }
    }
    perm_free(perm);
    (void)unlink(stats_path);
    (void)unsetenv("TNY_TEST_MANIFEST_STATS");
    (void)unsetenv("TNY_TEST_MANIFEST_LINEAGE");
    (void)unsetenv("TNY_TEST_MANIFEST_DESTINATION");
    return 0;
}
