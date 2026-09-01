/* test_runner.c — session-runner wire plumbing (docs/adr/0053): the unix
 * listener, the NDJSON client parser under arbitrary split boundaries
 * (real transports split anywhere — AGENTS.md), socket-path fallback for
 * deep home directories, and the isolation switch. The full fork lifecycle
 * is covered end to end by tests/integration/test_isolation.py. */
#include "greatest.h"
#include "core/runner.h"
#include "net/net.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* Every runner→client message shape on one wire, in order. */
static const char *WIRE =
    "{\"ev\":\"hello\",\"pid\":4242,\"provider\":\"openai\",\"model\":\"m1\","
    "\"session_id\":\"0123456789abcdef\",\"turn_active\":true}\n"
    "{\"ev\":\"snapshot\",\"text\":\"partial so far\"}\n"
    "{\"ev\":\"text_delta\",\"text\":\"hi ✓ there\\n\"}\n"
    "this line is not json and must be skipped\n"
    "{\"ev\":\"tool_start\",\"tool_name\":\"list_files\",\"tool_id\":\"t1\","
    "\"detail\":\"{\\\"path\\\": \\\".\\\"}\"}\n"
    "{\"ev\":\"tool_end\",\"tool_name\":\"list_files\",\"tool_id\":\"t1\","
    "\"detail\":\"3 entries\",\"ok\":true}\n"
    "{\"ev\":\"permission\",\"id\":\"p9\",\"summary\":\"run rm -rf\",\"options\":5}\n"
    "{\"ev\":\"usage\",\"in\":11,\"out\":7,\"context_used\":100,"
    "\"context_size\":200000,\"cost\":0.25}\n"
    "{\"ev\":\"steer_rejected\",\"text\":\"do the tests instead\"}\n"
    "{\"ev\":\"error\",\"text\":\"boom\",\"code\":\"io\"}\n"
    "{\"ev\":\"turn_end\",\"stop\":\"done\",\"exit_code\":0,"
    "\"result_text\":\"{\\\"output\\\":\\\"x\\\"}\\n\"}\n"
    "{\"ev\":\"bye\",\"text\":\"runner exiting\"}\n";

typedef struct {
    int server_fd;
    tny_runner_client *client;
    char sock[256];
} wire_env;

static int wire_begin(wire_env *w) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(w->sock, sizeof w->sock, "%s/tny-rn-test-%ld.sock", tmp, (long)getpid());
    int lfd = unix_listen(w->sock);
    if (lfd < 0) return -1;
    w->client = tny_runner_client_connect(w->sock, 2000);
    if (!w->client) {
        close(lfd);
        return -1;
    }
    w->server_fd = accept(lfd, NULL, NULL);
    close(lfd);
    return w->server_fd < 0 ? -1 : 0;
}

static void wire_end(wire_env *w) {
    if (w->server_fd >= 0) close(w->server_fd);
    tny_runner_client_close(w->client);
    unlink(w->sock);
}

/* Drain the queue into an array; caller frees each message. */
static int wire_collect(tny_runner_client *c, tny_runner_msg **out, int cap) {
    int n = 0;
    tny_runner_msg *m;
    while (n < cap && (m = tny_runner_client_pop(c))) out[n++] = m;
    return n;
}

static enum greatest_test_res wire_assert_sequence(wire_env *w) {
    tny_runner_msg *ms[16];
    int n = wire_collect(w->client, ms, 16);
    ASSERT_EQ_FMT(11, n, "%d");

    ASSERT_EQ(TNY_RMSG_HELLO, ms[0]->kind);
    ASSERT_EQ_FMT(4242L, (long)ms[0]->pid, "%ld");
    ASSERT_STR_EQ("openai", ms[0]->provider);
    ASSERT_STR_EQ("m1", ms[0]->model);
    ASSERT(ms[0]->turn_active);

    ASSERT_EQ(TNY_RMSG_SNAPSHOT, ms[1]->kind);
    ASSERT_STR_EQ("partial so far", ms[1]->text);

    ASSERT_EQ(TNY_RMSG_EVENT, ms[2]->kind);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ms[2]->ev.kind);
    ASSERT_STR_EQ("hi ✓ there\n", ms[2]->ev.text);
    ASSERT_EQ_FMT((long)strlen("hi ✓ there\n"), (long)ms[2]->ev.text_len, "%ld");

    /* the garbage line was skipped without dropping the connection */
    ASSERT_EQ(TNY_RMSG_EVENT, ms[3]->kind);
    ASSERT_EQ(TNY_EV_TOOL_START, ms[3]->ev.kind);
    ASSERT_STR_EQ("list_files", ms[3]->ev.tool_name);
    ASSERT_STR_EQ("{\"path\": \".\"}", ms[3]->ev.tool_detail);

    ASSERT_EQ(TNY_EV_TOOL_END, ms[4]->ev.kind);
    ASSERT(ms[4]->ev.tool_ok);
    ASSERT_STR_EQ("3 entries", ms[4]->ev.tool_detail);

    ASSERT_EQ(TNY_EV_PERMISSION, ms[5]->ev.kind);
    ASSERT_STR_EQ("p9", ms[5]->ev.perm_id);
    ASSERT_STR_EQ("run rm -rf", ms[5]->ev.perm_summary);
    ASSERT_EQ_FMT(5, ms[5]->ev.perm_options, "%d");

    ASSERT_EQ(TNY_EV_USAGE, ms[6]->ev.kind);
    ASSERT_EQ_FMT(11LL, (long long)ms[6]->ev.in_tokens, "%lld");
    ASSERT_EQ_FMT(7LL, (long long)ms[6]->ev.out_tokens, "%lld");
    ASSERT_EQ_FMT(200000LL, (long long)ms[6]->ev.context_size, "%lld");
    ASSERT(ms[6]->ev.has_cost);
    ASSERT_IN_RANGE(0.25, ms[6]->ev.cost, 1e-9);

    ASSERT_EQ(TNY_EV_STEER_REJECTED, ms[7]->ev.kind);
    ASSERT_STR_EQ("do the tests instead", ms[7]->ev.text);

    ASSERT_EQ(TNY_EV_ERROR, ms[8]->ev.kind);
    ASSERT_STR_EQ("boom", ms[8]->ev.text);
    ASSERT_EQ(TNY_EVENT_ERROR_IO, ms[8]->ev.error_code);

    ASSERT_EQ(TNY_RMSG_TURN_END, ms[9]->kind);
    ASSERT_EQ(TNY_STOP_DONE, ms[9]->ev.stop);
    ASSERT_EQ_FMT(0, ms[9]->exit_code, "%d");
    ASSERT_STR_EQ("{\"output\":\"x\"}\n", ms[9]->result_json);

    ASSERT_EQ(TNY_RMSG_BYE, ms[10]->kind);
    ASSERT_STR_EQ("runner exiting", ms[10]->text);

    for (int i = 0; i < n; i++) tny_runner_msg_free(ms[i]);
    PASS();
}

TEST runner_wire_whole_buffer(void) {
    wire_env w;
    ASSERT_EQ(0, wire_begin(&w));
    ASSERT_EQ((ssize_t)strlen(WIRE), write(w.server_fd, WIRE, strlen(WIRE)));
    ASSERT_EQ(0, tny_runner_client_pump(w.client));
    enum greatest_test_res res = wire_assert_sequence(&w);
    wire_end(&w);
    return res;
}

TEST runner_wire_survives_every_split_boundary(void) {
    /* byte-at-a-time is the harshest split; a few mid-size chunks cover the
     * rest. Real transports split anywhere (AGENTS.md). */
    size_t lens[] = {1, 3, 7, 64};
    for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        wire_env w;
        ASSERT_EQ(0, wire_begin(&w));
        size_t total = strlen(WIRE);
        for (size_t off = 0; off < total; off += lens[li]) {
            size_t n = lens[li] < total - off ? lens[li] : total - off;
            ASSERT_EQ((ssize_t)n, write(w.server_fd, WIRE + off, n));
            ASSERT_EQ(0, tny_runner_client_pump(w.client));
        }
        enum greatest_test_res res = wire_assert_sequence(&w);
        wire_end(&w);
        if (res != GREATEST_TEST_RES_PASS) return res;
    }
    PASS();
}

TEST runner_client_ops_reach_the_server(void) {
    wire_env w;
    ASSERT_EQ(0, wire_begin(&w));
    const char *imgs[] = {"/tmp/a.png", NULL};
    ASSERT_EQ(0, tny_runner_client_turn(w.client, "do \"things\"", imgs, true));
    ASSERT_EQ(0, tny_runner_client_steer(w.client, "steer text"));
    ASSERT_EQ(0, tny_runner_client_cancel(w.client, true));
    ASSERT_EQ(0, tny_runner_client_perm(w.client, "p1", TNY_PERM_DECISION_ALLOW_ALWAYS));
    ASSERT_EQ(0, tny_runner_client_end(w.client, "exit"));
    char buf[2048];
    size_t got = 0;
    while (got < sizeof buf - 1) {
        ssize_t n = read(w.server_fd, buf + got, sizeof buf - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        if (strstr(buf, "\"op\":\"end\"")) break;
    }
    buf[got] = 0;
    ASSERT(strstr(buf, "{\"op\":\"turn\",\"prompt\":\"do \\\"things\\\"\","
                       "\"images\":[\"/tmp/a.png\"],\"continue_recovery\":true}\n"));
    ASSERT(strstr(buf, "{\"op\":\"steer\",\"text\":\"steer text\"}\n"));
    ASSERT(strstr(buf, "{\"op\":\"cancel\",\"hard\":true}\n"));
    ASSERT(strstr(buf, "{\"op\":\"perm\",\"id\":\"p1\",\"decision\":\"allow_always\"}\n"));
    ASSERT(strstr(buf, "{\"op\":\"end\",\"reason\":\"exit\"}\n"));
    wire_end(&w);
    PASS();
}

TEST runner_client_reports_server_gone(void) {
    wire_env w;
    ASSERT_EQ(0, wire_begin(&w));
    const char *line = "{\"ev\":\"status\",\"text\":\"one last word\"}\n";
    ASSERT_EQ((ssize_t)strlen(line), write(w.server_fd, line, strlen(line)));
    close(w.server_fd);
    w.server_fd = -1;
    ASSERT_EQ(-1, tny_runner_client_pump(w.client)); /* EOF reported... */
    tny_runner_msg *m = tny_runner_client_pop(w.client);
    ASSERT(m); /* ...but queued messages stay poppable */
    ASSERT_EQ(TNY_RMSG_EVENT, m->kind);
    ASSERT_EQ(TNY_EV_STATUS, m->ev.kind);
    ASSERT_STR_EQ("one last word", m->ev.text);
    tny_runner_msg_free(m);
    wire_end(&w);
    PASS();
}

TEST runner_sock_path_falls_back_for_deep_dirs(void) {
    /* short dir: the socket lives beside lock/pid in the session dir */
    char *p = tny_runner_sock_path("/tmp/x");
    ASSERT(p);
    ASSERT_STR_EQ("/tmp/x/sock", p);
    free(p);
    /* deep dir: per-uid runtime dir keyed by the session id (basename) */
    char deep[300] = "/tmp/";
    memset(deep + 5, 'd', 250);
    memcpy(deep + 255, "/0123456789abcdef", sizeof "/0123456789abcdef");
    char *fb = tny_runner_sock_path(deep);
    ASSERT(fb);
    ASSERT(str_ends(fb, "/0123456789abcdef.sock"));
    ASSERT(strlen(fb) < 100);
    free(fb);
    PASS();
}

TEST runner_isolation_switch(void) {
    const char *prev = getenv("TNY_ISOLATE");
    unsetenv("TNY_ISOLATE");
    ASSERT(tny_isolation_enabled(NULL));
    setenv("TNY_ISOLATE", "0", 1);
    ASSERT_FALSE(tny_isolation_enabled(NULL));
    if (prev) setenv("TNY_ISOLATE", prev, 1);
    else unsetenv("TNY_ISOLATE");
    PASS();
}

SUITE(runner_suite) {
    RUN_TEST(runner_wire_whole_buffer);
    RUN_TEST(runner_wire_survives_every_split_boundary);
    RUN_TEST(runner_client_ops_reach_the_server);
    RUN_TEST(runner_client_reports_server_gone);
    RUN_TEST(runner_sock_path_falls_back_for_deep_dirs);
    RUN_TEST(runner_isolation_switch);
}
