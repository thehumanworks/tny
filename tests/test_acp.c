/* test_acp.c — ACP client transport seams (docs/adr/0017): agent-URL
 * detection, JSON-RPC message builders, and pollfd assembly. Pure logic
 * only; lifecycle and wire behavior live in tests/integration/test_acp*.sh. */
#include "greatest.h"
#include "backends/acp/acp_client.h"

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
        snprintf(capture->tool_detail, sizeof capture->tool_detail, "%s",
                 ev->tool_detail);
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

SUITE(acp_suite) {
    RUN_TEST(agent_is_ws_detects_only_ws_urls);
    RUN_TEST(fmt_builders_produce_exact_json);
    RUN_TEST(transport_pollfds_stdio);
    RUN_TEST(transport_pollfds_respects_max_and_missing_fds);
    RUN_TEST(usage_update_preserves_context_accounting);
    RUN_TEST(sparse_tool_update_emits_progress);
}
