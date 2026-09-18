/* Real filesystem/flock/fork admission tests. No mocked ownership or locks. */
#include "core/admission.h"
#include "util/jobs_host.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fault only publication and the directory sync after a successful rename.
 * All normal operations use the real OS; wrappers are linked by the runner. */
static bool fail_rename, fail_directory_sync;
int tny_admission_test_rename(const char *from, const char *to);
int tny_admission_test_fsync(int fd);
int tny_admission_test_rename(const char *from, const char *to) {
    if (fail_rename) {
        errno = EIO;
        return -1;
    }
    return rename(from, to);
}
int tny_admission_test_fsync(int fd) {
    if (fail_directory_sync) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
static tny_admission_scope scope;
static tny_admission_attempt attempt = {"run", 0, 1};
static tny_admission_result apply(tny_admission_op op) {
    tny_admission_result r;
    for (int i = 0; i < 5000; i++) {
        assert(tny_admission_apply(&scope, &attempt, op, false, true, &r) == 0);
        if (r.reason != TNY_ADMISSION_BUSY) return r;
        tny_jobs_host_sleep_ms(1);
    }
    assert(!"transaction deadline");
    return r;
}
static void init(const char *label, uint32_t cap, uint32_t queue, uint64_t limit) {
    scope.label = label;
    scope.cap = cap;
    scope.queue_cap = queue;
    scope.claim_limit = limit;
    attempt = (tny_admission_attempt){"run", 0, 1};
    assert(apply(TNY_ADMISSION_INIT).reason == TNY_ADMISSION_READY);
}
static void reaped(pid_t pid) {
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void write_byte(int fd, char value) { assert(write(fd, &value, 1) == 1); }
static char read_byte(int fd) {
    char value;
    assert(read(fd, &value, 1) == 1);
    return value;
}
static void basic(void) {
    init("basic", 1, 2, 3);
    tny_admission_result r = apply(TNY_ADMISSION_CLAIM);
    assert(r.reason == TNY_ADMISSION_GRANTED && r.claims == 1 && r.active == 1);
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_OWNED);
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_RELEASE, false, false, &r) == EPERM);
    attempt.task = 1;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_QUEUED_CAPACITY);
    attempt.task = 2;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_QUEUED_CAPACITY);
    attempt.task = 3;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_QUEUE_FULL);
    attempt.task = 0;
    assert(apply(TNY_ADMISSION_RELEASE).reason == TNY_ADMISSION_RELEASED);
    attempt.task = 2;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_QUEUED_FIFO);
    attempt.task = 1;
    assert(apply(TNY_ADMISSION_CANCEL).reason == TNY_ADMISSION_CANCELED);
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_CANCELED);
    attempt.task = 2;
    r = apply(TNY_ADMISSION_CLAIM);
    assert(r.reason == TNY_ADMISSION_GRANTED && r.claims == 2);
    assert(apply(TNY_ADMISSION_CANCEL).reason == TNY_ADMISSION_CLEANUP_HOLD);
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_CLEANUP_HOLD);
    assert(apply(TNY_ADMISSION_RELEASE).reason == TNY_ADMISSION_RELEASED);
    assert(apply(TNY_ADMISSION_RELEASE).claims == 2);
    attempt.attempt = 2;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_GRANTED);
    apply(TNY_ADMISSION_RELEASE);
    attempt.task = 4;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_EXHAUSTED);
    /* Cancel-before-enqueue persists a tombstone: later claim cannot launch. */
    assert(apply(TNY_ADMISSION_CANCEL).reason == TNY_ADMISSION_CANCELED);
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_CANCELED);
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, true, false, &r) == EDEADLK);
    scope.cap = 2;
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, false, false, &r) == EINVAL);
    scope.cap = 1;
    scope.provider_scope = "Bearer secret/key";
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_INIT, false, false, &r) == EINVAL);
    scope.provider_scope = "fixture-account";
}
static void pause_death(void) {
    init("death", 1, 4, 8);
    int p[2];
    assert(pipe(p) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(p[0]);
        assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_GRANTED);
        write_byte(p[1], 'g');
        raise(SIGSTOP);
        _exit(0);
    }
    close(p[1]);
    assert(read_byte(p[0]) == 'g');
    close(p[0]);
    int status;
    assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    attempt.task = 1;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_QUEUED_CAPACITY);
    assert(kill(pid, SIGKILL) == 0);
    assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
    /* A reaped owner does not prove its launch or descendants were cleaned. */
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_QUEUED_CAPACITY);
    attempt.task = 0;
    assert(apply(TNY_ADMISSION_HOLD).reason == TNY_ADMISSION_CLEANUP_HOLD);
    assert(apply(TNY_ADMISSION_CLAIM).claims == 1);
    /* This fixture launched no subprocess; parent has exact cleanup proof. */
    apply(TNY_ADMISSION_RELEASE);
    attempt.task = 1;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_GRANTED);
    apply(TNY_ADMISSION_RELEASE);
}
static void cancel_race(void) {
    init("race", 1, 4, 100);
    for (uint32_t i = 0; i < 40; i++) {
        attempt.task = i;
        int p[2];
        assert(pipe(p) == 0);
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            close(p[1]);
            read_byte(p[0]);
            tny_admission_result r = apply(TNY_ADMISSION_CLAIM);
            assert(r.reason == TNY_ADMISSION_GRANTED || r.reason == TNY_ADMISSION_CANCELED);
            _exit(0);
        }
        close(p[0]);
        write_byte(p[1], 'x');
        close(p[1]);
        tny_admission_result r = apply(TNY_ADMISSION_CANCEL);
        assert(r.reason == TNY_ADMISSION_CANCELED || r.reason == TNY_ADMISSION_CLEANUP_HOLD);
        reaped(pid);
        r = apply(TNY_ADMISSION_INSPECT);
        assert(r.reason == TNY_ADMISSION_CANCELED || r.reason == TNY_ADMISSION_CLEANUP_HOLD);
        if (r.reason == TNY_ADMISSION_CLEANUP_HOLD) apply(TNY_ADMISSION_RELEASE);
        assert(apply(TNY_ADMISSION_CLAIM).reason != TNY_ADMISSION_GRANTED);
    }
}
static void contenders(void) {
    init("contenders", 2, 32, 32);
    int p[2], gate[2];
    assert(pipe(p) == 0 && pipe(gate) == 0);
    pid_t children[12];
    for (uint32_t i = 0; i < 12; i++) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (!children[i]) {
            close(p[0]);
            close(gate[1]);
            attempt.task = i;
            bool granted = false;
            for (int tries = 0; tries < 5000; tries++) {
                tny_admission_result r = apply(TNY_ADMISSION_CLAIM);
                if (r.reason == TNY_ADMISSION_GRANTED) {
                    granted = true;
                    break;
                }
                assert(r.reason == TNY_ADMISSION_QUEUED_CAPACITY ||
                       r.reason == TNY_ADMISSION_QUEUED_FIFO);
                tny_jobs_host_sleep_ms(1);
            }
            assert(granted);
            write_byte(p[1], '+');
            read_byte(gate[0]);
            close(gate[0]);
            tny_jobs_host_sleep_ms(20);
            write_byte(p[1], '-');
            assert(apply(TNY_ADMISSION_RELEASE).reason == TNY_ADMISSION_RELEASED);
            _exit(0);
        }
    }
    close(p[1]);
    close(gate[0]);
    /* Deterministic overlap: neither of the first two holders can finish yet. */
    assert(read_byte(p[0]) == '+' && read_byte(p[0]) == '+');
    for (int i = 0; i < 12; i++) write_byte(gate[1], 'x');
    close(gate[1]);
    int active = 2, peak = 2;
    for (int i = 0; i < 22; i++) {
        active += read_byte(p[0]) == '+' ? 1 : -1;
        assert(active >= 0 && active <= 2);
        if (active > peak) peak = active;
    }
    close(p[0]);
    for (size_t i = 0; i < 12; i++) reaped(children[i]);
    assert(active == 0 && peak == 2);
    tny_admission_result r = apply(TNY_ADMISSION_INSPECT);
    assert(r.claims == 12 && r.active == 0 && r.queued == 0);
}
static void replay_and_exhaustion(void) {
    init("replay", 1, 4, 1);
    int p[2];
    assert(pipe(p) == 0);
    pid_t children[8];
    for (size_t i = 0; i < 8; i++) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (!children[i]) {
            close(p[0]);
            tny_admission_result r = apply(TNY_ADMISSION_CLAIM);
            assert(r.claims == 1);
            assert(r.reason == TNY_ADMISSION_GRANTED || r.reason == TNY_ADMISSION_OWNED);
            write_byte(p[1], r.reason == TNY_ADMISSION_GRANTED ? 'g' : 'o');
            _exit(0);
        }
    }
    close(p[1]);
    int grants = 0;
    for (size_t i = 0; i < 8; i++) {
        if (read_byte(p[0]) == 'g') grants++;
        reaped(children[i]);
    }
    close(p[0]);
    assert(grants == 1);
    apply(TNY_ADMISSION_RELEASE);
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_RELEASED);
    attempt.task = 1;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_EXHAUSTED);
    init("exhaustion", 1, 4, 2);
    apply(TNY_ADMISSION_CLAIM);
    attempt.task = 1;
    apply(TNY_ADMISSION_CLAIM);
    attempt.task = 2;
    apply(TNY_ADMISSION_CLAIM);
    attempt.task = 0;
    apply(TNY_ADMISSION_RELEASE);
    attempt.task = 1;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_GRANTED);
    apply(TNY_ADMISSION_RELEASE);
    attempt.task = 2;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_EXHAUSTED);
    assert(apply(TNY_ADMISSION_CANCEL).reason == TNY_ADMISSION_CANCELED);
    scope.provider_scope = "another-account";
    apply(TNY_ADMISSION_INIT);
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_GRANTED);
    scope.provider_scope = "fixture-account";
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_CANCELED);
}
static void publication_faults(void) {
    init("faults", 1, 4, 8);
    tny_admission_result r;
    fail_rename = true;
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, false, false, &r) == EIO);
    fail_rename = false;
    r = apply(TNY_ADMISSION_INSPECT);
    assert(r.reason == TNY_ADMISSION_NOT_FOUND && r.claims == 0);
    fail_directory_sync = true;
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, false, false, &r) == EIO);
    fail_directory_sync = false;
    r = apply(TNY_ADMISSION_CLAIM);
    assert(r.reason == TNY_ADMISSION_OWNED && r.claims == 1 && r.active == 1);
    apply(TNY_ADMISSION_HOLD);
    fail_rename = true;
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_RELEASE, false, true, &r) == EIO);
    fail_rename = false;
    assert(apply(TNY_ADMISSION_INSPECT).reason == TNY_ADMISSION_CLEANUP_HOLD);
    apply(TNY_ADMISSION_RELEASE);
}
static void storage(void) {
    init("storage", 2, 2, 2048);
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/storage/fixture-account/state.lock", scope.root);
    assert(n > 0 && (size_t)n < sizeof(path));
    int lock = tny_jobs_host_lock_open(path);
    assert(lock >= 0 && tny_jobs_host_lock_try(lock) == TNY_JOBS_LOCK_ACQUIRED);
    tny_admission_result r;
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, false, false, &r) == 0);
    assert(r.reason == TNY_ADMISSION_BUSY);
    tny_jobs_host_lock_close(lock);
    for (uint32_t i = 0; i < TNY_ADMISSION_HISTORY_MAX; i++) {
        attempt.task = i;
        assert(apply(TNY_ADMISSION_CANCEL).reason == TNY_ADMISSION_CANCELED);
    }
    attempt.task = TNY_ADMISSION_HISTORY_MAX;
    assert(apply(TNY_ADMISSION_CLAIM).reason == TNY_ADMISSION_HISTORY_FULL);
    n = snprintf(path, sizeof(path), "%s/storage/fixture-account/state.json", scope.root);
    assert(n > 0 && (size_t)n < sizeof(path));
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(tny_jobs_host_write_private(path, "{}", 2) == 0);
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, false, false, &r) == EIO);
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_INIT, false, false, &r) == EIO);
    assert(unlink(path) == 0);
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_CLAIM, false, false, &r) == ENOENT);
    assert(symlink("state.lock", path) == 0);
    assert(tny_admission_apply(&scope, &attempt, TNY_ADMISSION_INIT, false, false, &r) != 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    alarm(60);
    scope.root = argv[1];
    scope.provider_scope = "fixture-account";
    basic();
    pause_death();
    cancel_race();
    contenders();
    replay_and_exhaustion();
    publication_faults();
    storage();
    puts("admission: FIFO, bounds, budget, replay, cancel races, pause/death, 12 processes, "
         "persistence passed");
    return 0;
}
