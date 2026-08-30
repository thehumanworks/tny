#include "greatest.h"

#include "broker/broker.h"
#include "broker/client.h"
#include "broker/protocol.h"
#include "vt/vt.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    ASSERT_EQ(0, tt_broker_client_detach(&client, id));

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
    ASSERT_EQ(0, tt_broker_client_snapshot_begin(&client, id, 0));
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

    /* Local GUI control can explicitly kill even after re-attaching. */
    ASSERT_EQ(0, tt_broker_client_attach(&client, id));
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
