/* Production ACP bridge -> fresh execution server -> native permission path.
 * A bounded real prompt wait outlasts the requested Lua execution budget. */
#include "core/acp_bridge.h"
#include "core/execution.h"
#include "lib/custom_tools.h"
#include "util/execution_command.h"
#include "util/process.h"
#include "util/tny_poll.h"
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

/* Include the actual bridge to exercise its private call admission/settlement
 * without changing its clock or replacing an executor. */
#include "../../src/core/acp_bridge.c"

static unsigned prompts, custom_invocations;
static tny_perm_decision prompt_decision;
static int64_t prompt_wait_ms;

static tny_perm_decision blocking_prompt(const char *tool, const char *summary, void *ud) {
    (void)ud;
    REQUIRE(strcmp(tool, "write_file") == 0 && summary);
    ++prompts;
    int64_t started = monotonic_ms(), deadline = started + 500;
    while (monotonic_ms() < deadline) (void)tny_poll(NULL, 0, 10);
    prompt_wait_ms += monotonic_ms() - started;
    return prompt_decision;
}
static int32_t TNY_CALL unsupported_async(void *ud, tny_tool_call *call, uint64_t generation,
                                          tny_bytes arguments, tny_tool_result_v1 *result) {
    (void)ud;
    (void)call;
    (void)generation;
    (void)arguments;
    (void)result;
    ++custom_invocations;
    return TNY_TOOL_INVOKE_ASYNC;
}
static tny_bytes bytes(const char *s) { return (tny_bytes){s, strlen(s)}; }

static void bridge_init(tny_acp_bridge *bridge, tny_ctx *ctx, int pair[2]) {
    memset(bridge, 0, sizeof(*bridge));
    bridge->env.ctx = ctx;
    bridge->env.session = session_new(ctx);
    bridge->env.perm = perm_new(ctx);
    REQUIRE(bridge->env.session && bridge->env.perm);
    bridge->active = true;
    for (int i = 0; i < BRIDGE_CLIENTS; ++i) bridge->clients[i].fd = -1;
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    REQUIRE(set_nonblock(pair[0], true) == 0 && set_nonblock(pair[1], true) == 0);
    bridge->clients[0].fd = pair[0];
}
static void bridge_free(tny_acp_bridge *bridge, int peer) {
    client_close(&bridge->clients[0]);
    close(peer);
    perm_free(bridge->env.perm);
    session_close(bridge->env.session);
}
static void invoke_code(tny_acp_bridge *bridge, const char *code, int timeout_ms) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    REQUIRE(d);
    yyjson_mut_val *root = yyjson_mut_obj(d), *args = yyjson_mut_obj(d);
    REQUIRE(root && args);
    yyjson_mut_doc_set_root(d, root);
    REQUIRE(yyjson_mut_obj_add_str(d, root, "name", "run_code"));
    REQUIRE(yyjson_mut_obj_add_val(d, root, "arguments", args));
    REQUIRE(yyjson_mut_obj_add_strcpy(d, args, "code", code));
    REQUIRE(yyjson_mut_obj_add_int(d, args, "timeout_ms", timeout_ms));
    yyjson_doc *params = yyjson_mut_doc_imut_copy(d, NULL);
    REQUIRE(params);
    REQUIRE(call_tool(bridge, &bridge->clients[0], "1", yyjson_doc_get_root(params)) == 0);
    yyjson_doc_free(params);
    yyjson_mut_doc_free(d);
    REQUIRE(!bridge->clients[0].pending && !bridge->clients[0].permission);
}
static void response_contains(tny_acp_bridge *bridge, int peer, const char *expected) {
    REQUIRE(drain_client(bridge, &bridge->clients[0]) == 0);
    char response[8192];
    ssize_t n = read(peer, response, sizeof(response) - 1);
    REQUIRE(n > 0);
    response[n] = 0;
    if (!strstr(response, expected)) fprintf(stderr, "unexpected fixture response: %s\n", response);
    REQUIRE(strstr(response, expected));
}
static void permission_case(tny_ctx *ctx, bool prompt, bool allow) {
    tny_acp_bridge bridge;
    int pair[2];
    bridge_init(&bridge, ctx, pair);
    bridge.env.prompt = prompt ? blocking_prompt : NULL;
    prompt_decision = allow ? TNY_PERM_DECISION_ALLOW : TNY_PERM_DECISION_DENY;
    prompts = 0;
    prompt_wait_ms = 0;
    char *path = path_join(ctx->cwd, "approved.txt");
    REQUIRE(path);
    (void)unlink(path);
    invoke_code(
        &bridge,
        "print(tools.call('write_file', '{\"path\":\"approved.txt\",\"content\":\"approved\"}')); "
        "print('EXECUTION_FINISHED')",
        250);
    if (prompts != (prompt ? 1u : 0u))
        fprintf(stderr, "missing prompt response: %s\n", bridge.clients[0].output.data);
    REQUIRE(prompts == (prompt ? 1u : 0u));
    if (prompt) REQUIRE(prompt_wait_ms >= 500);
    if (prompt && allow) {
        size_t len = 0;
        char *content = file_slurp(path, &len);
        REQUIRE(content && len == 8 && strcmp(content, "approved") == 0);
        free(content);
        REQUIRE(!bridge.env.perm_blocked);
        response_contains(&bridge, pair[1], "EXECUTION_FINISHED");
    } else {
        REQUIRE(access(path, F_OK) != 0 && bridge.env.perm_blocked);
        response_contains(&bridge, pair[1], "permission");
    }
    (void)unlink(path);
    free(path);
    bridge_free(&bridge, pair[1]);
}
static void reject_direct_and_custom(tny_ctx *ctx) {
    tny_acp_bridge bridge;
    int pair[2];
    bridge_init(&bridge, ctx, pair);
    const char *wire =
        "{\"name\":\"write_file\",\"arguments\":{\"path\":\"forbidden.txt\",\"content\":\"x\"}}";
    yyjson_doc *direct = jparse(wire, strlen(wire));
    REQUIRE(direct);
    REQUIRE(call_tool(&bridge, &bridge.clients[0], "1", yyjson_doc_get_root(direct)) == 0);
    response_contains(&bridge, pair[1], "only run_code is exposed");
    yyjson_doc_free(direct);
    ctx->custom_tools = custom_tools_new();
    REQUIRE(ctx->custom_tools);
    tny_tool_spec_v1 spec = {.abi_version = TNY_TOOL_SPEC_ABI_VERSION,
                             .struct_size = sizeof(spec),
                             .name = bytes("async_fixture"),
                             .description = bytes("must never be invoked"),
                             .input_schema_json = bytes("{\"type\":\"object\",\"properties\":{}}"),
                             .sensitivity = TNY_TOOL_SENSITIVITY_SAFE,
                             .invoke = unsupported_async};
    tny_tool_registration *registration = NULL;
    REQUIRE(custom_tools_register(ctx->custom_tools, NULL, &spec, &registration) == TNY_STATUS_OK);
    invoke_code(&bridge, "print(tools.call('async_fixture', '{}'))", 250);
    response_contains(&bridge, pair[1], "no direct fallback");
    REQUIRE(custom_invocations == 0);
    REQUIRE(custom_tools_unregister(registration) == TNY_STATUS_OK);
    custom_tools_free(ctx->custom_tools);
    ctx->custom_tools = NULL;
    bridge_free(&bridge, pair[1]);
}
static void execution_budget_case(tny_ctx *ctx) {
    tny_acp_bridge bridge;
    int pair[2];
    bridge_init(&bridge, ctx, pair);
    invoke_code(&bridge, "while true do end", 250);
    response_contains(&bridge, pair[1], "error:");
    bridge_free(&bridge, pair[1]);
}
int main(int argc, char **argv) {
    REQUIRE(tny_process_scope_admit() == 0);
    if (argc == 2 && strcmp(argv[1], "--exec-server") == 0) return tny_execution_server_main();
    if (argc == 2 && strcmp(argv[1], "--exec-command") == 0) return tny_exec_command_main();
    REQUIRE(argc == 2);
    tny_ctx *ctx = tny_ctx_new_explicit(argv[1], argv[1]);
    REQUIRE(ctx);
    /* This fixture is a real native executable with the private server entries. */
    ctx->library_mode = false;
    ctx->perm_mode = TNY_MODE_ASK;
    ctx->no_save = true;
    permission_case(ctx, true, true);
    permission_case(ctx, true, false);
    permission_case(ctx, false, false);
    reject_direct_and_custom(ctx);
    execution_budget_case(ctx);
    tny_ctx_free(ctx);
    puts("PASS production ACP run_code excludes human approval time, preserves denial and runtime "
         "bounds, rejects direct calls and unsupported custom async without invocation");
    return 0;
}
