/* test_acp.c — ACP client transport seams (docs/adr/0017): agent-URL
 * detection, JSON-RPC message builders, and pollfd assembly. Pure logic
 * only; lifecycle and wire behavior live in tests/integration/test_acp*.sh. */
#include "greatest.h"
#include "backends/acp/acp_client.h"

#include <string.h>

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
    ASSERT_STR_EQ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
                  b.data);
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

SUITE(acp_suite) {
    RUN_TEST(agent_is_ws_detects_only_ws_urls);
    RUN_TEST(fmt_builders_produce_exact_json);
    RUN_TEST(transport_pollfds_stdio);
    RUN_TEST(transport_pollfds_respects_max_and_missing_fds);
}
