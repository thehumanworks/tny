/* Real bridge admission/permission/settlement with a deterministic clock and a
 * controllable async executor. No production timeout override, sleeps or agents. */
#include "core/acp_bridge.h"
#include "util/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUIRE(expr)                                                                    \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            fprintf(stderr, "ACP deadline assertion at line %d: %s\n", __LINE__, #expr); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

static int64_t clock_ms;
static unsigned executions;
static bool ready;
static tny_perm_decision prompt_decision;

static int64_t fixture_clock(void) { return clock_ms; }
static char *fixture_execute(tools_env *env, tools_call *call) {
    (void)env;
    REQUIRE(call && call->name);
    executions++;
    return NULL;
}
static bool fixture_pending(const tools_call *call) { return call != NULL; }
static int fixture_take_async(tools_call *call, char **result, bool *is_error) {
    REQUIRE(call && call->name);
    if (!ready) return 0;
    *result = xstrdup("fixture completed");
    *is_error = false;
    return 1;
}

#define monotonic_ms          fixture_clock
#define tools_call_execute    fixture_execute
#define tools_call_pending    fixture_pending
#define tools_call_take_async fixture_take_async
#include "../../src/core/acp_bridge.c"
#undef monotonic_ms
#undef tools_call_execute
#undef tools_call_pending
#undef tools_call_take_async

static tny_perm_decision blocking_prompt(const char *tool, const char *summary, void *ud) {
    (void)ud;
    REQUIRE(strcmp(tool, "write_file") == 0 && summary);
    clock_ms += 2 * BRIDGE_TIMEOUT_MS;
    return prompt_decision;
}

static void drain(tny_acp_bridge *bridge, bridge_client *client, int peer, const char *expected) {
    REQUIRE(drain_client(bridge, client) == 0);
    char response[8192];
    ssize_t bytes = read(peer, response, sizeof response - 1);
    if (!expected) {
        REQUIRE(bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    } else {
        REQUIRE(bytes > 0);
        response[bytes] = 0;
        REQUIRE(strstr(response, expected));
    }
}

static void run_case(tny_ctx *ctx, bool blocking, bool denied, bool finish) {
    tny_acp_bridge bridge = {0};
    bridge.env.ctx = ctx;
    bridge.env.session = session_new(ctx);
    bridge.env.perm = perm_new(ctx);
    REQUIRE(bridge.env.session && bridge.env.perm);
    bridge.active = true;
    for (int i = 0; i < BRIDGE_CLIENTS; i++) bridge.clients[i].fd = -1;
    if (blocking) bridge.env.prompt = blocking_prompt;
    prompt_decision = denied ? TNY_PERM_DECISION_DENY : TNY_PERM_DECISION_ALLOW;
    clock_ms = 1000;
    executions = 0;
    ready = false;
    int pair[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    REQUIRE(set_nonblock(pair[0], true) == 0 && set_nonblock(pair[1], true) == 0);
    bridge_client *client = &bridge.clients[0];
    client->fd = pair[0];
    const char *arguments =
        "{\"name\":\"write_file\",\"arguments\":{\"path\":\"blocked.txt\",\"content\":\"x\"}}";
    yyjson_doc *doc = jparse(arguments, strlen(arguments));
    REQUIRE(doc);
    REQUIRE(call_tool(&bridge, client, "1", yyjson_doc_get_root(doc)) == 0);
    yyjson_doc_free(doc);
    if (!blocking) {
        REQUIRE(client->pending && client->permission && executions == 0);
        /* Repeated dispatch while a human takes longer than two entire tool
         * execution windows must preserve both the prompt and request. */
        for (int i = 0; i < 3; i++) {
            clock_ms += BRIDGE_TIMEOUT_MS;
            drain(&bridge, client, pair[1], NULL);
            REQUIRE(client->pending && client->permission && executions == 0);
        }
        char permission_id[64];
        snprintf(permission_id, sizeof permission_id, "%s", client->tool_id);
        REQUIRE(tny_acp_bridge_respond_permission(&bridge, permission_id, prompt_decision));
        REQUIRE(!tny_acp_bridge_respond_permission(&bridge, permission_id, prompt_decision));
    }
    if (denied) {
        REQUIRE(executions == 0 && !client->pending && bridge.env.perm_blocked);
        drain(&bridge, client, pair[1], "permission denied");
    } else {
        REQUIRE(executions == 1 && client->pending && !client->permission);
        REQUIRE(client->deadline == clock_ms + BRIDGE_TIMEOUT_MS);
        clock_ms += BRIDGE_TIMEOUT_MS - 1;
        drain(&bridge, client, pair[1], NULL);
        REQUIRE(client->pending);
        if (finish) {
            ready = true;
            drain(&bridge, client, pair[1], "fixture completed");
        } else {
            clock_ms++;
            drain(&bridge, client, pair[1], "ACP tool call timed out");
        }
        REQUIRE(!client->pending && !client->permission);
    }
    char *path = path_join(ctx->cwd, "blocked.txt");
    REQUIRE(path && access(path, F_OK) != 0);
    free(path);
    client_close(client);
    close(pair[1]);
    perm_free(bridge.env.perm);
    session_close(bridge.env.session);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    tny_ctx *ctx = tny_ctx_new_explicit(argv[1], argv[1]);
    REQUIRE(ctx);
    ctx->perm_mode = TNY_MODE_ASK;
    for (int blocking = 0; blocking <= 1; blocking++) {
        run_case(ctx, blocking != 0, false, true);
        run_case(ctx, blocking != 0, false, false);
        run_case(ctx, blocking != 0, true, false);
    }
    tny_ctx_free(ctx);
    puts("PASS ACP parked and blocking approval exclude human waiting; fresh execution "
         "deadlines, async completion, execution timeout and denial");
    return 0;
}
