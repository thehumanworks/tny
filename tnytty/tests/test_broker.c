#include "greatest.h"

#include "broker/broker.h"
#include "broker/client.h"
#include "broker/protocol.h"
#include "vt/vt.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

TEST http_response_parser_survives_every_split(void) {
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello";
    size_t len = sizeof response - 1;
    for (size_t split = 0; split <= len; split++) {
        tt_http_response_parser p;
        tt_http_response_parser_init(&p);
        int first = tt_http_response_parser_feed(&p, response, split);
        ASSERT(first == 0 || (split == len && first == 1));
        if (split < len)
            ASSERT_EQ(1, tt_http_response_parser_feed(&p, response + split, len - split));
        size_t body_len = 0;
        const unsigned char *body = tt_http_response_body(&p, &body_len);
        ASSERT_EQ(5u, body_len);
        ASSERT_MEM_EQ("hello", body, 5);
        tt_http_response_parser_free(&p);
    }
    PASS();
}

TEST http_response_parser_rejects_malformed_and_extra_bytes(void) {
    tt_http_response_parser p;
    tt_http_response_parser_init(&p);
    const char *missing = "HTTP/1.1 200 OK\r\n\r\n";
    ASSERT_EQ(-1, tt_http_response_parser_feed(&p, missing, strlen(missing)));
    ASSERT_EQ(EPROTO, errno);
    tt_http_response_parser_free(&p);

    tt_http_response_parser_init(&p);
    const char *extra = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxx";
    ASSERT_EQ(-1, tt_http_response_parser_feed(&p, extra, strlen(extra)));
    ASSERT_EQ(EPROTO, errno);
    tt_http_response_parser_free(&p);
    PASS();
}

TEST broker_snapshot_uses_the_canonical_vt_format(void) {
    vt *source = vt_new(7, 3, 0);
    vt *copy = vt_new(1, 1, 0);
    ASSERT(source != NULL);
    ASSERT(copy != NULL);
    vt_feed(source, "hello", 5);
    size_t len = vt_snapshot_size(source), written = 0;
    void *wire = malloc(len);
    ASSERT(wire != NULL);
    ASSERT_EQ(0, vt_snapshot_write(source, wire, len, &written));
    ASSERT_EQ(len, written);
    ASSERT_EQ(0, vt_snapshot_read(copy, wire, len));
    ASSERT_EQ(7, vt_cols(copy));
    ASSERT_EQ(3, vt_rows(copy));
    ASSERT_EQ(vt_generation(source), vt_generation(copy));
    char line[32];
    vt_line_text(copy, 0, line, sizeof line);
    ASSERT_STR_EQ("hello", line);
    ((unsigned char *)wire)[0] ^= 1;
    ASSERT_EQ(-1, vt_snapshot_read(copy, wire, len));
    free(wire);
    vt_free(copy);
    vt_free(source);
    PASS();
}

static int start_test_broker(const char *path, pid_t *pid_out) {
    int ready[2];
    if (pipe(ready) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(ready[0]);
        _exit(tt_broker_run(path, ready[1]));
    }
    close(ready[1]);
    struct pollfd pfd = {ready[0], POLLIN, 0};
    char state = 0;
    if (poll(&pfd, 1, 3000) > 0) {
        ssize_t ignored = read(ready[0], &state, 1);
        (void)ignored;
    }
    close(ready[0]);
    if (state != '1') {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    *pid_out = pid;
    return 0;
}

static int reserve_tcp_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof sa;
    int port = getsockname(fd, (struct sockaddr *)&sa, &len) == 0 ? ntohs(sa.sin_port) : -1;
    close(fd);
    return port;
}

static int tcp_request(int port, const char *method, const char *path, const char *token,
                       tt_http_response_parser *response) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    char request[512];
    int n = snprintf(request, sizeof request,
                     "%s %s HTTP/1.1\r\nHost: localhost\r\n%s%s%sContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     method, path, token ? "Authorization: Bearer " : "", token ? token : "",
                     token ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof request || write(fd, request, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    tt_http_response_parser_init(response);
    char buf[4096];
    int result = 0;
    while (!result) {
        ssize_t got = read(fd, buf, sizeof buf);
        if (got <= 0) break;
        result = tt_http_response_parser_feed(response, buf, (size_t)got);
    }
    close(fd);
    return result == 1 ? 0 : -1;
}

TEST broker_detach_keeps_session_and_kill_removes_it(void) {
    char dir[] = "/tmp/tnytty-broker-test.XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);
    char path[104];
    snprintf(path, sizeof path, "%s/daemon.sock", dir);
    ASSERT_EQ(0, tt_broker_prepare_socket(path, NULL, 0));
    pid_t broker = -1;
    ASSERT_EQ(0, start_test_broker(path, &broker));

    struct stat st;
    ASSERT_EQ(0, stat(dir, &st));
    ASSERT_EQ(0700, st.st_mode & 0777);
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0600, st.st_mode & 0777);

    tt_broker_client client;
    char err[256];
    ASSERT_EQ(0, tt_broker_client_open(&client, path, err, sizeof err));
    char *argv[] = {(char *)"sh", (char *)"-c", (char *)"printf '%d detached' $$; sleep 30", NULL};
    char id[9];
    ASSERT_EQ(0, tt_broker_client_create(&client, argv, NULL, 40, 4, id));
    ASSERT_EQ(8u, strlen(id));
    ASSERT_EQ(0, tt_broker_client_attach(&client, id));
    /* Red-close used to race this in-flight snapshot: detach was skipped
     * and the durable session stayed attached after the GUI exited. */
    ASSERT_EQ(0, tt_broker_client_snapshot_begin(&client, id, 0));
    ASSERT_EQ(0, tt_broker_client_detach(&client, id));
    tt_buf attached_state;
    ASSERT_EQ(0, tt_broker_client_list(&client, &attached_state));
    ASSERT(strstr(attached_state.data, "\"attached\":false") != NULL);
    tt_buf_free(&attached_state);

    bool saw_output = false;
    pid_t child_pid = -1;
    for (int i = 0; i < 100 && !saw_output; i++) {
        tt_buf body;
        if (tt_broker_client_snapshot(&client, id, 0, &body) == 0) {
            vt *mirror = vt_new(1, 1, 0);
            if (mirror && vt_snapshot_read(mirror, body.data, body.len) == 0) {
                char line[128];
                vt_line_text(mirror, 0, line, sizeof line);
                saw_output = strstr(line, "detached") != NULL;
                if (saw_output) ASSERT_EQ(1, sscanf(line, "%d", &child_pid));
            }
            vt_free(mirror);
            tt_buf_free(&body);
        }
        if (!saw_output) poll(NULL, 0, 10);
    }
    ASSERT(saw_output);
    ASSERT(child_pid > 0);
    ASSERT_EQ(0, kill(child_pid, 0)); /* detach preserved the exact child */
    const void *async_body = NULL;
    size_t async_len = 0;
    int async_rc = 0;
    for (int i = 0; i < 50 && async_rc == 0; i++) {
        struct pollfd pfd;
        ASSERT_EQ(1, tt_broker_client_pollfd(&client, &pfd));
        ASSERT_GTE(poll(&pfd, 1, 100), 0);
        async_rc = tt_broker_client_pump(&client, pfd.revents, &async_body, &async_len);
    }
    ASSERT_EQ(1, async_rc);
    vt *async_mirror = vt_new(1, 1, 0);
    ASSERT(async_mirror != NULL);
    ASSERT_EQ(0, vt_snapshot_read(async_mirror, async_body, async_len));
    vt_free(async_mirror);
    tt_buf list;
    ASSERT_EQ(0, tt_broker_client_list(&client, &list));
    ASSERT(strstr(list.data, id) != NULL);
    tt_buf_free(&list);

    int port = reserve_tcp_port();
    ASSERT(port > 0);
    bool public_auth = false;
    ASSERT_EQ(0, tt_broker_client_listen(&client, "127.0.0.1", port, "test-token", &public_auth,
                                         err, sizeof err));
    ASSERT(public_auth);
    /* A later GUI that omitted an auto-generated token reuses the live
     * listener without unknowably rotating its credential. */
    ASSERT_EQ(0, tt_broker_client_listen(&client, "127.0.0.1", port, NULL, &public_auth, err,
                                         sizeof err));
    ASSERT(public_auth);
    /* Configuring the same listener is idempotent. */
    ASSERT_EQ(0, tt_broker_client_listen(&client, "127.0.0.1", port, "test-token", &public_auth,
                                         err, sizeof err));
    int conflicting_port = reserve_tcp_port();
    ASSERT(conflicting_port > 0);
    ASSERT_EQ(-1, tt_broker_client_listen(&client, "127.0.0.1", conflicting_port, "test-token",
                                          &public_auth, err, sizeof err));
    ASSERT_EQ(EBUSY, errno);
    tt_http_response_parser public_response;
    ASSERT_EQ(0, tcp_request(port, "GET", "/v1/sessions", NULL, &public_response));
    ASSERT_EQ(401, public_response.status);
    tt_http_response_parser_free(&public_response);
    ASSERT_EQ(0, tcp_request(port, "GET", "/v1/sessions", "test-token", &public_response));
    ASSERT_EQ(200, public_response.status);
    size_t public_len = 0;
    const unsigned char *public_body = tt_http_response_body(&public_response, &public_len);
    ASSERT(public_body != NULL);
    ASSERT(public_len > strlen(id));
    ASSERT(strstr((const char *)public_body, id) != NULL);
    tt_http_response_parser_free(&public_response);

    /* Local GUI control can explicitly kill even after re-attaching. */
    ASSERT_EQ(0, tt_broker_client_attach(&client, id));
    char session_path[64];
    snprintf(session_path, sizeof session_path, "/v1/sessions/%s", id);
    ASSERT_EQ(0, tcp_request(port, "DELETE", session_path, "test-token", &public_response));
    ASSERT_EQ(409, public_response.status); /* public callers cannot race the GUI lifetime */
    tt_http_response_parser_free(&public_response);
    ASSERT_EQ(0, tt_broker_client_kill(&client, id));
    for (int i = 0; i < 100 && kill(child_pid, 0) == 0; i++) poll(NULL, 0, 10);
    ASSERT_EQ(-1, kill(child_pid, 0));
    ASSERT_EQ(ESRCH, errno);
    ASSERT_EQ(0, tt_broker_client_list(&client, &list));
    ASSERT(strstr(list.data, id) == NULL);
    tt_buf_free(&list);
    tt_broker_client_close(&client);

    kill(broker, SIGTERM);
    ASSERT_EQ(broker, waitpid(broker, NULL, 0));
    unlink(path);
    rmdir(dir);
    PASS();
}

SUITE(broker_suite) {
    RUN_TEST(http_response_parser_survives_every_split);
    RUN_TEST(http_response_parser_rejects_malformed_and_extra_bytes);
    RUN_TEST(broker_snapshot_uses_the_canonical_vt_format);
    RUN_TEST(broker_detach_keeps_session_and_kill_removes_it);
}
