/* test_session.c — the pending-input queue. A pty master takes only a
 * kernel buffer's worth of input at a time; everything past that used to
 * be dropped on EAGAIN, which cuts sequences in half (the ESC of
 * ESC[201~ vanishing leaves a literal "[201~" at the prompt). These
 * tests drive a real pty whose reader is paused, then drain. */
#include "greatest.h"
#include "session/session.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define BIG (64 * 1024)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Raw mode from the master side: no canonical MAX_CANON truncation and
 * no echo, so what the child reads is exactly what we wrote. */
static void make_raw(int master) {
    struct termios tio;
    if (tcgetattr(master, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(master, TCSANOW, &tio);
    }
}

/* Drain the queue the way every event loop does: poll for POLLOUT, then
 * tt_session_flush. Returns true once nothing is pending. */
static bool drain(tt_session *s, double budget) {
    double deadline = now_sec() + budget;
    while (tt_session_pending(s) > 0 && now_sec() < deadline) {
        struct pollfd pfd = {s->pty.master, POLLOUT, 0};
        if (poll(&pfd, 1, 50) < 0 && errno != EINTR) break;
        if (pfd.revents & POLLOUT)
            if (tt_session_flush(s) < 0) break;
    }
    return tt_session_pending(s) == 0;
}

static off_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

/* A paused reader (`sleep` before `cat`) guarantees the pty buffer fills
 * and the queue takes over. Every byte must still arrive, in order. */
TEST paused_reader_loses_no_input(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/tnytty-queue-%d.bin", (int)getpid());
    unlink(path);
    char cmd[192];
    snprintf(cmd, sizeof cmd, "sleep 1; cat > %s", path);
    char *argv[] = {(char *)"sh", (char *)"-c", cmd, NULL};

    tt_registry reg;
    tt_registry_init(&reg, 0);
    tt_session *s = tt_session_create(&reg, argv, 80, 24);
    ASSERT(s != NULL);
    make_raw(s->pty.master);

    char *payload = malloc(BIG);
    ASSERT(payload != NULL);
    for (int i = 0; i < BIG; i++) payload[i] = (char)(i % 251);

    /* Two writes: the first fills the pty and overflows into the queue,
     * the second must land behind it, not in front of it. */
    ASSERT_EQ(BIG / 2, tt_session_write(s, payload, BIG / 2));
    ASSERT(tt_session_pending(s) > 0); /* the queue did the catching */
    ASSERT_EQ(BIG / 2, tt_session_write(s, payload + BIG / 2, BIG / 2));

    ASSERT(drain(s, 10.0));

    double deadline = now_sec() + 10.0;
    while (file_size(path) < BIG && now_sec() < deadline) poll(NULL, 0, 20);
    ASSERT_EQ_FMT((long long)BIG, (long long)file_size(path), "%lld");

    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    char *got = malloc(BIG);
    ASSERT(got != NULL);
    ASSERT_EQ((size_t)BIG, fread(got, 1, BIG, f));
    fclose(f);
    ASSERT_MEM_EQ(payload, got, BIG);

    free(got);
    free(payload);
    unlink(path);
    tt_session_destroy(&reg, s);
    tt_registry_free(&reg);
    PASS();
}

/* The queue is bounded: past the cap a write is rejected whole, with
 * ENOBUFS, and the bytes already queued are untouched. */
TEST queue_cap_rejects_whole_writes(void) {
    char *argv[] = {(char *)"sleep", (char *)"5", NULL}; /* never reads */
    tt_registry reg;
    tt_registry_init(&reg, 0);
    tt_session *s = tt_session_create(&reg, argv, 80, 24);
    ASSERT(s != NULL);
    make_raw(s->pty.master);

    size_t chunk = 256 * 1024;
    char *buf = calloc(1, chunk);
    ASSERT(buf != NULL);
    size_t accepted = 0;
    int rc = 0;
    while ((rc = tt_session_write(s, buf, chunk)) > 0) {
        accepted += (size_t)rc;
        ASSERT(accepted <= TT_INPUT_QUEUE_MAX + chunk);
    }
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(ENOBUFS, errno);
    ASSERT(accepted >= TT_INPUT_QUEUE_MAX - chunk);
    size_t held = tt_session_pending(s);
    ASSERT(held > 0);
    ASSERT(held <= TT_INPUT_QUEUE_MAX);
    /* A rejected write queues nothing at all. */
    ASSERT_EQ(-1, tt_session_write(s, buf, chunk));
    ASSERT_EQ(held, tt_session_pending(s));
    /* Zero-length is a no-op, and small writes still fail at the cap. */
    ASSERT_EQ(0, tt_session_write(s, buf, 0));

    free(buf);
    tt_session_destroy(&reg, s);
    tt_registry_free(&reg);
    PASS();
}

/* Nothing pending, nothing to flush; a dead session refuses input. */
TEST flush_and_write_on_a_dead_session(void) {
    char *argv[] = {(char *)"true", NULL};
    tt_registry reg;
    tt_registry_init(&reg, 0);
    tt_session *s = tt_session_create(&reg, argv, 80, 24);
    ASSERT(s != NULL);
    ASSERT_EQ(0u, (unsigned)tt_session_pending(s));
    ASSERT_EQ(0, tt_session_flush(s));

    char scratch[256];
    double deadline = now_sec() + 5.0;
    while (s->alive && now_sec() < deadline) {
        struct pollfd pfd = {s->pty.master, POLLIN, 0};
        poll(&pfd, 1, 50);
        tt_session_pump(s, scratch, sizeof scratch, NULL, NULL);
    }
    ASSERT_FALSE(s->alive);
    ASSERT_EQ(-1, tt_session_write(s, "x", 1));
    ASSERT_EQ(ENXIO, errno);
    ASSERT_EQ(0, tt_session_flush(s));

    tt_session_destroy(&reg, s);
    tt_registry_free(&reg);
    PASS();
}

SUITE(session_suite) {
    RUN_TEST(paused_reader_loses_no_input);
    RUN_TEST(queue_cap_rejects_whole_writes);
    RUN_TEST(flush_and_write_on_a_dead_session);
}
