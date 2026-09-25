/* Real framed transport tests; protocol parsing is tested separately. */
#include "greatest.h"
#include "util/execution_host.h"
#include "core/execution_protocol.h"
#include "util/util.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int transport_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) return -1;
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
#ifdef SO_NOSIGPIPE
        int one = 1;
        if (setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one)) goto fail;
#endif
    }
    return 0;
fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

static void transport_header(unsigned char header[4], size_t length) {
    header[0] = (unsigned char)(length >> 24);
    header[1] = (unsigned char)(length >> 16);
    header[2] = (unsigned char)(length >> 8);
    header[3] = (unsigned char)length;
}

TEST execution_transport_roundtrip(void) {
    int fds[2];
    ASSERT_EQ(0, transport_pair(fds));
    const char *json = "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"result\":{\"ok\":true}}";
    int64_t deadline = monotonic_ms() + 1000;
    ASSERT_EQ(0, tny_exec_host_send(fds[0], json, deadline, NULL, NULL));
    char *received = tny_exec_host_receive(fds[1], deadline, NULL, NULL);
    ASSERT(received);
    ASSERT_STR_EQ(json, received);
    free(received);
    close(fds[0]);
    close(fds[1]);
    PASS();
}

TEST execution_transport_every_split_boundary(void) {
    const char *json = "{\"result\":\"split boundaries include the length header\"}";
    size_t length = strlen(json);
    unsigned char frame[128];
    transport_header(frame, length);
    memcpy(frame + 4, json, length);
    for (size_t split = 1; split < length + 4; ++split) {
        int fds[2];
        ASSERT_EQ(0, transport_pair(fds));
        pid_t child = fork();
        ASSERT(child >= 0);
        if (!child) {
            close(fds[1]);
            if (write(fds[0], frame, split) != (ssize_t)split) _exit(2);
            usleep(2000);
            size_t remaining = length + 4 - split;
            if (write(fds[0], frame + split, remaining) != (ssize_t)remaining) _exit(3);
            close(fds[0]);
            _exit(0);
        }
        close(fds[0]);
        char *received = tny_exec_host_receive(fds[1], monotonic_ms() + 2000, NULL, NULL);
        close(fds[1]);
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        ASSERT_EQ(child, waited);
        ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        ASSERT(received);
        ASSERT_STR_EQ(json, received);
        free(received);
    }
    PASS();
}

TEST execution_transport_truncated_frames(void) {
    const unsigned char frames[][8] = {
        {0, 0, 0},                 /* EOF in header */
        {0, 0, 0, 3, '{'},         /* EOF in body */
        {0, 0, 0, 3, '{', 0, '}'}, /* raw NUL */
    };
    const size_t lengths[] = {3, 5, 7};
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i) {
        int fds[2];
        ASSERT_EQ(0, transport_pair(fds));
        ASSERT_EQ((ssize_t)lengths[i], write(fds[0], frames[i], lengths[i]));
        close(fds[0]);
        char *received = tny_exec_host_receive(fds[1], monotonic_ms() + 1000, NULL, NULL);
        close(fds[1]);
        ASSERT_EQ(NULL, received);
    }
    PASS();
}

TEST execution_transport_rejects_lengths_before_payload(void) {
    const size_t lengths[] = {0, TNY_EXEC_FRAME_MAX + 1, UINT32_MAX};
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i) {
        int fds[2];
        ASSERT_EQ(0, transport_pair(fds));
        unsigned char frame[4];
        transport_header(frame, lengths[i]);
        ASSERT_EQ(4, write(fds[0], frame, sizeof frame));
        errno = 0;
        char *received = tny_exec_host_receive(fds[1], monotonic_ms() + 1000, NULL, NULL);
        int error = errno;
        close(fds[0]);
        close(fds[1]);
        ASSERT_EQ(NULL, received);
        ASSERT_EQ(EMSGSIZE, error);
    }
    PASS();
}

static bool transport_cancel(void *userdata) {
    unsigned *checks = userdata;
    return ++*checks >= 2;
}

TEST execution_transport_stalled_frame_deadline_and_cancel(void) {
    for (int cancel = 0; cancel < 2; ++cancel) {
        int fds[2];
        ASSERT_EQ(0, transport_pair(fds));
        unsigned char partial[] = {0, 0};
        ASSERT_EQ(2, write(fds[0], partial, sizeof partial));
        unsigned checks = 0;
        int64_t before = monotonic_ms();
        char *received =
            tny_exec_host_receive(fds[1], before + 60, cancel ? transport_cancel : NULL, &checks);
        int error = errno;
        int64_t elapsed = monotonic_ms() - before;
        close(fds[0]);
        close(fds[1]);
        ASSERT_EQ(NULL, received);
        ASSERT_EQ(cancel ? ECANCELED : ETIMEDOUT, error);
        ASSERT(elapsed < 1500);
        if (cancel) ASSERT(checks >= 2);
    }
    PASS();
}

TEST execution_transport_max_frame_and_closed_peer(void) {
    int fds[2];
    ASSERT_EQ(0, transport_pair(fds));
    char *payload = malloc(TNY_EXEC_FRAME_MAX + 1);
    ASSERT(payload);
    memset(payload, 'x', TNY_EXEC_FRAME_MAX);
    payload[TNY_EXEC_FRAME_MAX] = 0;
    pid_t child = fork();
    ASSERT(child >= 0);
    if (!child) {
        close(fds[1]);
        int rc = tny_exec_host_send(fds[0], payload, monotonic_ms() + 5000, NULL, NULL);
        free(payload);
        close(fds[0]);
        _exit(rc ? 2 : 0);
    }
    close(fds[0]);
    char *received = tny_exec_host_receive(fds[1], monotonic_ms() + 5000, NULL, NULL);
    close(fds[1]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    ASSERT_EQ(child, waited);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ASSERT(received);
    ASSERT_EQ(TNY_EXEC_FRAME_MAX, strlen(received));
    ASSERT_EQ(0, memcmp(payload, received, TNY_EXEC_FRAME_MAX));
    free(payload);
    free(received);
    ASSERT_EQ(0, transport_pair(fds));
    close(fds[0]);
    ASSERT(tny_exec_host_disconnected(fds[1]));
    ASSERT_EQ(-1, tny_exec_host_send(fds[1], "{}", monotonic_ms() + 1000, NULL, NULL));
    close(fds[1]);
    PASS();
}
#endif

/* Keep allocator-only transport coverage in macOS leaks. The two fork-based
 * cases remain in the ordinary sanitizer suite and Linux Valgrind run. */
SUITE(execution_transport_suite) {
#ifndef __EMSCRIPTEN__
    RUN_TEST(execution_transport_roundtrip);
    RUN_TEST(execution_transport_truncated_frames);
    RUN_TEST(execution_transport_rejects_lengths_before_payload);
    RUN_TEST(execution_transport_stalled_frame_deadline_and_cancel);
#endif
}

SUITE(execution_transport_process_suite) {
#ifndef __EMSCRIPTEN__
    RUN_TEST(execution_transport_every_split_boundary);
    RUN_TEST(execution_transport_max_frame_and_closed_peer);
#endif
}
