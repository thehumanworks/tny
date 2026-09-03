/* test_runner.c — session-runner wire plumbing (docs/adr/0053): the unix
 * listener, the NDJSON client parser under arbitrary split boundaries
 * (real transports split anywhere — AGENTS.md), socket-path fallback for
 * deep home directories, and the isolation switch. The full fork lifecycle
 * is covered end to end by tests/integration/test_isolation.py. */
#include "greatest.h"
#include "core/runner.h"
#include "core/config.h"
#include "core/session.h"
#include "core/tools.h"
#include "net/net.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>

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
    "{\"ev\":\"ask_user\",\"id\":\"q17\",\"question\":\"Which branch?\"}\n"
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

/* Cygwin/MSYS AF_UNIX emulation completes connect() only after the server
 * accepts, so a single-threaded connect-then-accept deadlocks there. The
 * runner never has this shape (parent and runner are separate processes);
 * the test connects on a thread while this one accepts. */
static void *wire_connect_main(void *arg) {
    wire_env *w = arg;
    w->client = tny_runner_client_connect(w->sock, 4000, TNY_RUNNER_OWNER, false);
    return NULL;
}

static int wire_begin(wire_env *w) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(w->sock, sizeof w->sock, "%s/tny-rn-test-%ld.sock", tmp, (long)getpid());
    w->client = NULL;
    w->server_fd = -1;
    int lfd = unix_listen(w->sock);
    if (lfd < 0) return -1;
    pthread_t th;
    if (pthread_create(&th, NULL, wire_connect_main, w) != 0) {
        close(lfd);
        return -1;
    }
    for (int i = 0; i < 100 && w->server_fd < 0; i++) {
        struct pollfd pf = {lfd, POLLIN, 0};
        if (tny_poll(&pf, 1, 50) > 0) w->server_fd = accept(lfd, NULL, NULL);
    }
    pthread_join(th, NULL);
    close(lfd);
    if (w->server_fd < 0 || !w->client) {
        if (w->client) tny_runner_client_close(w->client);
        if (w->server_fd >= 0) close(w->server_fd);
        return -1;
    }
    return 0;
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
    ASSERT_EQ_FMT(12, n, "%d");

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

    ASSERT_EQ(TNY_RMSG_ASK_USER, ms[6]->kind);
    ASSERT_STR_EQ("q17", ms[6]->id);
    ASSERT_STR_EQ("Which branch?", ms[6]->text);

    ASSERT_EQ(TNY_EV_USAGE, ms[7]->ev.kind);
    ASSERT_EQ_FMT(11LL, (long long)ms[7]->ev.in_tokens, "%lld");
    ASSERT_EQ_FMT(7LL, (long long)ms[7]->ev.out_tokens, "%lld");
    ASSERT_EQ_FMT(200000LL, (long long)ms[7]->ev.context_size, "%lld");
    ASSERT(ms[7]->ev.has_cost);
    ASSERT_IN_RANGE(0.25, ms[7]->ev.cost, 1e-9);

    ASSERT_EQ(TNY_EV_STEER_REJECTED, ms[8]->ev.kind);
    ASSERT_STR_EQ("do the tests instead", ms[8]->ev.text);

    ASSERT_EQ(TNY_EV_ERROR, ms[9]->ev.kind);
    ASSERT_STR_EQ("boom", ms[9]->ev.text);
    ASSERT_EQ(TNY_EVENT_ERROR_IO, ms[9]->ev.error_code);

    ASSERT_EQ(TNY_RMSG_TURN_END, ms[10]->kind);
    ASSERT_EQ(TNY_STOP_DONE, ms[10]->ev.stop);
    ASSERT_EQ_FMT(0, ms[10]->exit_code, "%d");
    ASSERT_STR_EQ("{\"output\":\"x\"}\n", ms[10]->result_json);

    ASSERT_EQ(TNY_RMSG_BYE, ms[11]->kind);
    ASSERT_STR_EQ("runner exiting", ms[11]->text);

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
    ASSERT_EQ(0, tny_runner_client_ask_user_reply(w.client, "q1", "release/next"));
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
    ASSERT(strstr(buf, "{\"op\":\"hello\",\"role\":\"owner\"}\n"));
    ASSERT(strstr(buf, "{\"op\":\"steer\",\"text\":\"steer text\"}\n"));
    ASSERT(strstr(buf, "{\"op\":\"cancel\",\"hard\":true}\n"));
    ASSERT(strstr(buf, "{\"op\":\"perm\",\"id\":\"p1\",\"decision\":\"allow_always\"}\n"));
    ASSERT(strstr(buf, "{\"op\":\"ask_user_reply\",\"id\":\"q1\",\"answer\":\"release/next\"}\n"));
    ASSERT(strstr(buf, "{\"op\":\"end\",\"reason\":\"exit\"}\n"));
    wire_end(&w);
    PASS();
}

TEST runner_client_reports_server_gone(void) {
    wire_env w;
    ASSERT_EQ(0, wire_begin(&w));
    /* Drain the client's hello first, as a real runner would: Cygwin's
     * AF_UNIX emulation resets the connection when a peer closes with unread
     * input, which would discard the line written below. */
    for (int i = 0; i < 20; i++) {
        struct pollfd pf = {w.server_fd, POLLIN, 0};
        if (tny_poll(&pf, 1, 50) <= 0) break;
        char sink[512];
        if (read(w.server_fd, sink, sizeof sink) <= 0) break;
    }
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
    ASSERT(tny_isolation_policy(NULL, true));
    /* Once macOS has initialized SecureTransport, a fork-only child must
     * not enter CoreFoundation/SecTrust. The production wrapper supplies
     * the live transport state; this pure seam makes the gate portable. */
    ASSERT_FALSE(tny_isolation_policy(NULL, false));
    setenv("TNY_ISOLATE", "0", 1);
    ASSERT_FALSE(tny_isolation_policy(NULL, true));
    if (prev) setenv("TNY_ISOLATE", prev, 1);
    else unsetenv("TNY_ISOLATE");
    PASS();
}

TEST runner_roles_enforce_operation_allowlists(void) {
    ASSERT(tny_runner_role_allows(TNY_RUNNER_OWNER, "turn"));
    ASSERT(tny_runner_role_allows(TNY_RUNNER_OWNER, "ask_user_reply"));
    ASSERT_FALSE(tny_runner_role_allows(TNY_RUNNER_OWNER, "image_attach"));
    ASSERT(tny_runner_role_allows(TNY_RUNNER_OBSERVER, "detach"));
    ASSERT_FALSE(tny_runner_role_allows(TNY_RUNNER_OBSERVER, "cancel"));
    ASSERT_FALSE(tny_runner_role_allows(TNY_RUNNER_OBSERVER, "perm"));
    ASSERT(tny_runner_role_allows(TNY_RUNNER_TOOL, "ask_user"));
    ASSERT(tny_runner_role_allows(TNY_RUNNER_TOOL, "image_attach"));
    ASSERT_FALSE(tny_runner_role_allows(TNY_RUNNER_TOOL, "turn"));
    PASS();
}

typedef struct {
    char root[512];
    char workspace[560];
    char state[560];
    tny_ctx *ctx;
    tny_session_state *session;
    char *sock;
    pid_t pid;
} live_runner;

static int live_runner_begin(live_runner *x) {
    memset(x, 0, sizeof *x);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(x->root, sizeof x->root, "%s/tny-runner-live-XXXXXX", tmp);
    if (!mkdtemp(x->root)) return -1;
    snprintf(x->workspace, sizeof x->workspace, "%s/workspace", x->root);
    snprintf(x->state, sizeof x->state, "%s/state", x->root);
    if (mkdir_p(x->workspace) != 0 || mkdir_p(x->state) != 0) return -1;
    x->ctx = tny_ctx_new_explicit(x->workspace, x->state);
    if (!x->ctx) return -1;
    x->session = session_new(x->ctx);
    if (!x->session) return -1;
    tny_runner_opts opts = {0};
    opts.serve = true;
    char err[256];
    x->pid = tny_runner_spawn(x->ctx, x->session, &opts, err, sizeof err);
    if (x->pid <= 0) return -1;
    x->sock = tny_runner_sock_path(x->session->dir);
    return x->sock ? 0 : -1;
}

static void live_runner_end(live_runner *x) {
    if (x->pid > 0) {
        kill(x->pid, SIGTERM);
        waitpid(x->pid, NULL, 0);
    }
    free(x->sock);
    if (x->session) session_close(x->session);
    tny_ctx_free(x->ctx);
}

static tny_runner_msg *wait_runner_msg(tny_runner_client *c, tny_runner_msg_kind kind) {
    int64_t deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        struct pollfd pf = {tny_runner_client_fd(c), POLLIN, 0};
        if (tny_poll(&pf, 1, 50) > 0) tny_runner_client_pump(c);
        tny_runner_msg *m;
        while ((m = tny_runner_client_pop(c))) {
            if (m->kind == kind) return m;
            tny_runner_msg_free(m);
        }
    }
    return NULL;
}

static char *read_control_result(int fd, const char *id) {
    buf_t in;
    buf_init(&in);
    int64_t deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (tny_poll(&pf, 1, 50) <= 0) continue;
        char tmp[1024];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        buf_append(&in, tmp, (size_t)n);
        char *nl;
        while ((nl = memchr(in.data, '\n', in.len))) {
            size_t len = (size_t)(nl - in.data);
            char *line = xstrndup(in.data, len);
            buf_consume(&in, len + 1);
            yyjson_doc *doc = jparse(line, len);
            const char *rid = doc ? jget_str(yyjson_doc_get_root(doc), "id") : NULL;
            if (rid && strcmp(rid, id) == 0) {
                yyjson_doc_free(doc);
                buf_free(&in);
                return line;
            }
            yyjson_doc_free(doc);
            free(line);
        }
    }
    buf_free(&in);
    return NULL;
}

TEST runner_correlates_question_and_fails_closed_on_owner_disconnect(void) {
    live_runner x;
    ASSERT_EQ(0, live_runner_begin(&x));
    tny_runner_client *owner = tny_runner_client_connect(x.sock, 4000, TNY_RUNNER_OWNER, true);
    tny_runner_client *observer =
        tny_runner_client_connect(x.sock, 4000, TNY_RUNNER_OBSERVER, false);
    ASSERT(owner);
    ASSERT(observer);
    tny_runner_msg *m = wait_runner_msg(owner, TNY_RMSG_HELLO);
    ASSERT(m);
    tny_runner_msg_free(m);
    m = wait_runner_msg(observer, TNY_RMSG_HELLO);
    ASSERT(m);
    tny_runner_msg_free(m);

    /* The observer is connected but cannot mutate the runner. */
    ASSERT_EQ(0, tny_runner_client_cancel(observer, true));
    m = wait_runner_msg(observer, TNY_RMSG_EVENT);
    ASSERT(m);
    ASSERT_EQ(TNY_EV_ERROR, m->ev.kind);
    ASSERT(strstr(m->ev.text, "not allowed"));
    tny_runner_msg_free(m);

    int tool = unix_connect(x.sock);
    ASSERT(tool >= 0);
    const char *ask = "{\"op\":\"hello\",\"role\":\"tool\"}\n"
                      "{\"op\":\"ask_user\",\"id\":\"corr-17\","
                      "\"question\":\"Which branch?\"}\n";
    ASSERT_EQ((ssize_t)strlen(ask), write(tool, ask, strlen(ask)));
    m = wait_runner_msg(owner, TNY_RMSG_ASK_USER);
    ASSERT(m);
    ASSERT_STR_EQ("corr-17", m->id);
    ASSERT_STR_EQ("Which branch?", m->text);
    ASSERT_EQ(0, tny_runner_client_ask_user_reply(owner, m->id, "release/next"));
    tny_runner_msg_free(m);
    char *reply = read_control_result(tool, "corr-17");
    ASSERT(reply);
    ASSERT(strstr(reply, "\"answer\":\"release/next\""));
    free(reply);
    close(tool);

    tool = unix_connect(x.sock);
    ASSERT(tool >= 0);
    const char *ask2 = "{\"op\":\"hello\",\"role\":\"tool\"}\n"
                       "{\"op\":\"ask_user\",\"id\":\"corr-18\","
                       "\"question\":\"Still there?\"}\n";
    ASSERT_EQ((ssize_t)strlen(ask2), write(tool, ask2, strlen(ask2)));
    m = wait_runner_msg(owner, TNY_RMSG_ASK_USER);
    ASSERT(m);
    tny_runner_msg_free(m);
    tny_runner_client_close(owner);
    owner = NULL;
    reply = read_control_result(tool, "corr-18");
    ASSERT(reply);
    ASSERT(strstr(reply, "\"ok\":false"));
    ASSERT(strstr(reply, "disconnected"));
    free(reply);
    close(tool);
    tny_runner_client_close(observer);
    live_runner_end(&x);
    PASS();
}

typedef struct {
    int lfd;
    int cfd;
    buf_t in;
    int calls;
} fake_control;

static int fake_control_pump(void *ud, int timeout_ms) {
    fake_control *f = ud;
    f->calls++;
    struct pollfd pf[2];
    nfds_t n = 0;
    pf[n++] = (struct pollfd){f->lfd, POLLIN, 0};
    if (f->cfd >= 0) pf[n++] = (struct pollfd){f->cfd, POLLIN, 0};
    if (tny_poll(pf, n, timeout_ms) < 0) return -1;
    if (pf[0].revents & POLLIN) {
        f->cfd = accept(f->lfd, NULL, NULL);
        if (f->cfd >= 0) set_nonblock(f->cfd, true);
    }
    if (f->cfd >= 0) {
        char tmp[2048];
        ssize_t got;
        while ((got = read(f->cfd, tmp, sizeof tmp)) > 0) buf_append(&f->in, tmp, (size_t)got);
        char *nl;
        while ((nl = memchr(f->in.data, '\n', f->in.len))) {
            size_t len = (size_t)(nl - f->in.data);
            yyjson_doc *doc = jparse(f->in.data, len);
            buf_consume(&f->in, len + 1);
            yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
            const char *op = jget_str(root, "op");
            const char *id = jget_str(root, "id");
            if (op && strcmp(op, "ask_user") == 0 && id) {
                buf_t reply;
                buf_init(&reply);
                buf_appends(&reply, "{\"ev\":\"control_result\",\"id\":");
                jescape(&reply, id);
                buf_appends(&reply, ",\"ok\":true,\"answer\":\"blue\"}\n");
                ssize_t sent = write(f->cfd, reply.data, reply.len);
                buf_free(&reply);
                if (sent < 0) {
                    yyjson_doc_free(doc);
                    return -1;
                }
            }
            yyjson_doc_free(doc);
        }
    }
    return 0;
}

TEST runner_terminal_pumps_control_while_child_asks_user(void) {
    live_runner x;
    memset(&x, 0, sizeof x);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(x.root, sizeof x.root, "%s/tny-runner-shell-XXXXXX", tmp);
    ASSERT(mkdtemp(x.root));
    snprintf(x.workspace, sizeof x.workspace, "%s/workspace", x.root);
    snprintf(x.state, sizeof x.state, "%s/state", x.root);
    ASSERT_EQ(0, mkdir_p(x.workspace));
    ASSERT_EQ(0, mkdir_p(x.state));
    x.ctx = tny_ctx_new_explicit(x.workspace, x.state);
    ASSERT(x.ctx);
    x.session = session_new(x.ctx);
    ASSERT(x.session);

    char sock[560];
    snprintf(sock, sizeof sock, "%s/control.sock", x.root);
    fake_control f = {.lfd = unix_listen(sock), .cfd = -1};
    ASSERT(f.lfd >= 0);
    buf_init(&f.in);
    char cwd[4096];
    ASSERT(getcwd(cwd, sizeof cwd));
    char cli[4200];
    const char *tny_bin = getenv("TNY_BIN"); /* make leaks/valgrind build it elsewhere */
    if (tny_bin && *tny_bin) snprintf(cli, sizeof cli, "%s", tny_bin);
    else snprintf(cli, sizeof cli, "%s/build/tny", cwd);
    char command[4600];
    snprintf(command, sizeof command,
             "test \"$TNY_SESSION_ID\" = \"%s\" && \"%s\" ask-user \"favorite color?\"",
             x.session->id, cli);
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"command\":");
    jescape(&args, command);
    buf_appends(&args, ",\"timeout_s\":5}");
    yyjson_doc *doc = jparse(args.data, args.len);
    ASSERT(doc);
    tools_env env = {0};
    env.ctx = x.ctx;
    env.session = x.session;
    env.session_sock = sock;
    env.session_id = x.session->id;
    env.control_pump = fake_control_pump;
    env.control_pump_ud = &f;
    bool handled = false;
    char *result = tool_shell_execute(&env, "terminal", yyjson_doc_get_root(doc), &handled);
    ASSERT(handled);
    ASSERT(result);
    ASSERT(strstr(result, "exit code: 0"));
    ASSERT(strstr(result, "output:\nblue"));
    ASSERT(f.calls > 0);
    free(result);
    yyjson_doc_free(doc);
    buf_free(&args);
    if (f.cfd >= 0) close(f.cfd);
    close(f.lfd);
    unlink(sock);
    buf_free(&f.in);
    session_close(x.session);
    tny_ctx_free(x.ctx);
    PASS();
}

TEST runner_image_attach_validates_root_and_magic_before_queueing(void) {
    live_runner x;
    memset(&x, 0, sizeof x);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(x.root, sizeof x.root, "%s/tny-runner-image-XXXXXX", tmp);
    ASSERT(mkdtemp(x.root));
    snprintf(x.workspace, sizeof x.workspace, "%s/workspace", x.root);
    snprintf(x.state, sizeof x.state, "%s/state", x.root);
    ASSERT_EQ(0, mkdir_p(x.workspace));
    ASSERT_EQ(0, mkdir_p(x.state));
    x.ctx = tny_ctx_new_explicit(x.workspace, x.state);
    ASSERT(x.ctx);
    x.session = session_new(x.ctx);
    ASSERT(x.session);
    char inside[600], outside[600], invalid[600];
    snprintf(inside, sizeof inside, "%s/inside.png", x.workspace);
    snprintf(outside, sizeof outside, "%s/outside.png", x.state);
    snprintf(invalid, sizeof invalid, "%s/not-image.png", x.workspace);
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0};
    ASSERT_EQ(0, file_write_atomic(inside, png, sizeof png));
    ASSERT_EQ(0, file_write_atomic(outside, png, sizeof png));
    ASSERT_EQ(0, file_write_atomic(invalid, "not an image", 12));
    tools_env env = {0};
    env.ctx = x.ctx;
    env.session = x.session;
    char err[256];
    ASSERT_EQ(0, tools_queue_image(&env, inside, true, NULL, NULL, NULL, err, sizeof err));
    ASSERT_EQ(1, env.n_pending_images);
    ASSERT_EQ(0, tools_flush_images(&env, err, sizeof err));
    ASSERT_EQ(0, env.n_pending_images);
    ASSERT_EQ(-1, tools_queue_image(&env, outside, true, NULL, NULL, NULL, err, sizeof err));
    ASSERT(strstr(err, "outside the allowed roots"));
    ASSERT_EQ(-1, tools_queue_image(&env, invalid, true, NULL, NULL, NULL, err, sizeof err));
    ASSERT(strstr(err, "not a png/jpeg/gif/webp"));
    session_close(x.session);
    tny_ctx_free(x.ctx);
    PASS();
}

SUITE(runner_suite) {
    RUN_TEST(runner_wire_whole_buffer);
    RUN_TEST(runner_wire_survives_every_split_boundary);
    RUN_TEST(runner_client_ops_reach_the_server);
    RUN_TEST(runner_client_reports_server_gone);
    RUN_TEST(runner_sock_path_falls_back_for_deep_dirs);
    RUN_TEST(runner_isolation_switch);
    RUN_TEST(runner_roles_enforce_operation_allowlists);
    RUN_TEST(runner_correlates_question_and_fails_closed_on_owner_disconnect);
    RUN_TEST(runner_terminal_pumps_control_while_child_asks_user);
    RUN_TEST(runner_image_attach_validates_root_and_magic_before_queueing);
}
