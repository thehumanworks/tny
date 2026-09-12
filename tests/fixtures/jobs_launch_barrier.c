/* Deterministic local failure fixture: pause the first pipe creation, then
 * return EMFILE. Only the test-owned submitter loads this library. No provider
 * or environment values are recorded; the two paths are barrier markers. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdbool.h>

#ifdef TNY_FIXTURE_ANCESTRY
/* Linux-only executable linked to the real process implementation. Report a
 * harmless sibling as a child during enumeration, then expose its real PPID
 * at capture. This models changed ancestry without uncontrolled PID reuse.
 * Record and reject any attempted pidfd signal to that unrelated sentinel. */
#include <stdarg.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "util/process.h"

static pid_t ancestry_root, ancestry_sentinel;
static int sentinel_fd = -1, sentinel_opens, sentinel_closes, sentinel_signals;
static bool ancestry_injected;

FILE *__real_fopen(const char *path, const char *mode);
long __real_syscall(long number, ...);
int __real_close(int fd);
FILE *__wrap_fopen(const char *path, const char *mode);
long __wrap_syscall(long number, ...);
int __wrap_close(int fd);

FILE *__wrap_fopen(const char *path, const char *mode) {
    char expected[64];
    snprintf(expected, sizeof expected, "/proc/%ld/stat", (long)ancestry_sentinel);
    if (!ancestry_injected && strcmp(path, expected) == 0) {
        FILE *file = tmpfile();
        if (!file) return NULL;
        fprintf(file, "%ld (ancestry-sentinel) S %ld\n", (long)ancestry_sentinel,
                (long)ancestry_root);
        rewind(file);
        ancestry_injected = true;
        return file;
    }
    return __real_fopen(path, mode);
}

long __wrap_syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    long result;
    if (number == SYS_pidfd_open) {
        int pid = va_arg(args, int), flags = va_arg(args, int);
        result = __real_syscall(number, pid, flags);
        if (pid == ancestry_sentinel && result >= 0) {
            sentinel_fd = (int)result;
            sentinel_opens++;
        }
    } else if (number == SYS_pidfd_send_signal) {
        int fd = va_arg(args, int), sig = va_arg(args, int);
        void *info = va_arg(args, void *);
        int flags = va_arg(args, int);
        if (fd == sentinel_fd) {
            sentinel_signals++;
            errno = EPERM;
            result = -1;
        } else result = __real_syscall(number, fd, sig, info, flags);
    } else {
        errno = ENOSYS;
        result = -1;
    }
    va_end(args);
    return result;
}

int __wrap_close(int fd) {
    if (fd == sentinel_fd) sentinel_closes++;
    return __real_close(fd);
}

static pid_t sleeping_child(void) {
    pid_t pid = fork();
    if (!pid) {
        for (;;) pause();
    }
    return pid;
}

int main(void) {
    ancestry_root = sleeping_child();
    if (ancestry_root < 0) return 2;
    ancestry_sentinel = sleeping_child();
    if (ancestry_sentinel < 0) {
        kill(ancestry_root, SIGKILL);
        waitpid(ancestry_root, NULL, 0);
        return 2;
    }
    bool reaped = false;
    int status = 0;
    int result = tny_process_stop_owned_tree(ancestry_root, &status, &reaped);
    bool root_absent = kill(ancestry_root, 0) == -1 && errno == ESRCH;
    bool sentinel_alive =
        kill(ancestry_sentinel, 0) == 0 && waitpid(ancestry_sentinel, NULL, WNOHANG) == 0;
    bool passed = ancestry_injected && sentinel_opens == 1 && sentinel_closes == 1 &&
                  sentinel_signals == 0 && sentinel_alive && result == -1 && reaped && root_absent;
    printf("injected=%d opens=%d closes=%d signals=%d sentinel_alive=%d "
           "cleanup=%d reaped=%d root_absent=%d\n",
           ancestry_injected, sentinel_opens, sentinel_closes, sentinel_signals, sentinel_alive,
           result, reaped, root_absent);
    /* Both are direct children owned solely by this fixture. Never signal a
     * reaped PID, even during failure cleanup. */
    if (sentinel_alive) {
        kill(ancestry_sentinel, SIGKILL);
        waitpid(ancestry_sentinel, NULL, 0);
    }
    if (!reaped && waitpid(ancestry_root, NULL, WNOHANG) == 0) {
        kill(ancestry_root, SIGKILL);
        waitpid(ancestry_root, NULL, 0);
    }
    return passed ? 0 : 1;
}
#elif defined(TNY_FIXTURE_REAP_LOST)
/* The separate tree-helper case has no supervisor initialization: its own
 * descendants must be auto-reaped from exec. Target-scoped mode takes priority. */
__attribute__((constructor)) static void discard_helper_child_status(void) {
    const char *mode = getenv("TNY_FIXTURE_AUTOREAP_AT_EXEC");
    if (!mode || strcmp(mode, "1") != 0 || getenv("TNY_FIXTURE_AUTOREAP_TARGET")) return;
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    action.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGCHLD, &action, NULL);
    struct sigaction actual;
    bool active = sigaction(SIGCHLD, NULL, &actual) == 0 && actual.sa_handler == SIG_IGN &&
                  (actual.sa_flags & SA_NOCLDWAIT);
    const char *marker = getenv("TNY_FIXTURE_AUTOREAP_MARKER");
    int fd = marker ? open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
    if (fd >= 0) {
        (void)write(fd, active ? "active" : "error", active ? 6 : 5);
        close(fd);
    }
}

/* The supervisor restores SIGCHLD during initialization. Inject only at its
 * selected real child spawn, after that reset, so the kernel discards status. */
static int discard_child_status(pid_t *pid, const char *path,
                                const posix_spawn_file_actions_t *actions,
                                const posix_spawnattr_t *attr, char *const argv[],
                                char *const envp[]) {
    const char *target = getenv("TNY_FIXTURE_AUTOREAP_TARGET");
    const char *marker = getenv("TNY_FIXTURE_AUTOREAP_MARKER");
    bool selected = target && marker && strcmp(path, target) == 0;
    bool default_before = false;
    if (selected) {
        struct sigaction before;
        default_before = sigaction(SIGCHLD, NULL, &before) == 0 && before.sa_handler == SIG_DFL &&
                         !(before.sa_flags & SA_NOCLDWAIT);
        struct sigaction action = {0};
        action.sa_handler = SIG_IGN;
        action.sa_flags = SA_NOCLDWAIT;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGCHLD, &action, NULL) != 0) return errno;
    }
#ifdef __APPLE__
    int rc = posix_spawn(pid, path, actions, attr, argv, envp);
#else
    int (*original)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                    const posix_spawnattr_t *, char *const[], char *const[]) =
        dlsym(RTLD_NEXT, "posix_spawn");
    int rc = original ? original(pid, path, actions, attr, argv, envp) : ENOSYS;
#endif
    if (selected && rc == 0) {
        struct sigaction actual;
        bool active = sigaction(SIGCHLD, NULL, &actual) == 0 && actual.sa_handler == SIG_IGN &&
                      (actual.sa_flags & SA_NOCLDWAIT);
        int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            char text[192];
            int len =
                snprintf(text, sizeof text,
                         "{\"parent\":%ld,\"child\":%ld,\"default_before\":%s,\"active\":%s}\n",
                         (long)getpid(), (long)*pid, default_before ? "true" : "false",
                         active ? "true" : "false");
            if (len > 0 && (size_t)len < sizeof text) (void)write(fd, text, (size_t)len);
            close(fd);
        }
    }
    return rc;
}
#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    int (*replacement)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                       const posix_spawnattr_t *, char *const[], char *const[]);
    int (*original)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                    const posix_spawnattr_t *, char *const[], char *const[]);
} reap_spawn_interpose = {discard_child_status, posix_spawn};
#else
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    return discard_child_status(pid, path, actions, attr, argv, envp);
}
#endif
#elif defined(TNY_FIXTURE_STOP_READER)
static ssize_t stopped_read(int fd, void *buffer, size_t size) {
    static bool stopped;
    if (fd == STDIN_FILENO && !stopped) {
        stopped = true;
        const char *marker = getenv("TNY_FIXTURE_PIPE_READY");
        if (marker) {
            int out = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (out >= 0) {
                char text[32];
                int n = snprintf(text, sizeof text, "%ld", (long)getpid());
                (void)write(out, text, (size_t)n);
                close(out);
            }
        }
        (void)kill(getpid(), SIGSTOP);
    }
#ifdef __APPLE__
    return read(fd, buffer, size);
#else
    ssize_t (*original)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    return original ? original(fd, buffer, size) : -1;
#endif
}
#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    ssize_t (*replacement)(int, void *, size_t);
    ssize_t (*original)(int, void *, size_t);
} read_interpose = {stopped_read, read};
#else
ssize_t read(int fd, void *buffer, size_t size) { return stopped_read(fd, buffer, size); }
#endif
#elif defined(TNY_FIXTURE_IMAGE_ENV)
static int inspect_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                         const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    if (argv[0] && argv[1] && argv[2] && argv[3] && strcmp(argv[3], "image") == 0) {
        bool absent = true;
        for (size_t i = 0; envp[i]; i++)
            if (strncmp(envp[i], "LANG=", 5) == 0) absent = false;
        const char *marker = getenv("TNY_FIXTURE_PIPE_READY");
        int fd = marker ? open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
        if (fd >= 0) {
            (void)write(fd, absent ? "absent" : "present", absent ? 6 : 7);
            close(fd);
        }
    }
#ifdef __APPLE__
    /* dyld excludes references inside the interposer image itself. */
    return posix_spawn(pid, path, actions, attr, argv, envp);
#else
    int (*original)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                    const posix_spawnattr_t *, char *const[], char *const[]) =
        dlsym(RTLD_NEXT, "posix_spawn");
    return original ? original(pid, path, actions, attr, argv, envp) : ENOSYS;
#endif
}
#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    int (*replacement)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                       const posix_spawnattr_t *, char *const[], char *const[]);
    int (*original)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                    const posix_spawnattr_t *, char *const[], char *const[]);
} spawn_interpose = {inspect_spawn, posix_spawn};
#else
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    return inspect_spawn(pid, path, actions, attr, argv, envp);
}
#endif
#elif defined(TNY_FIXTURE_SNAPSHOT_COMMIT)
static int snapshot_rename(const char *from, const char *to) {
    const char *base = strrchr(to, '/');
    char snapshot[4096];
    if (base && strcmp(base, "/job.json") == 0 &&
        snprintf(snapshot, sizeof snapshot, "%.*s/attempt-1.json", (int)(base - to), to) > 0 &&
        access(snapshot, F_OK) == 0) {
        const char *ready = getenv("TNY_FIXTURE_PIPE_READY");
        const char *resume = getenv("TNY_FIXTURE_PIPE_RESUME");
        if (ready && resume) {
            int fd = open(ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) close(fd);
            struct timespec delay = {.tv_nsec = 10000000};
            for (int i = 0; i < 1000 && access(resume, F_OK) != 0; i++) nanosleep(&delay, NULL);
            errno = EIO;
            return -1;
        }
    }
    int (*original)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
    return original ? original(from, to) : -1;
}
#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    int (*replacement)(const char *, const char *);
    int (*original)(const char *, const char *);
} rename_interpose = {snapshot_rename, rename};
#else
int rename(const char *from, const char *to) { return snapshot_rename(from, to); }
#endif
#else
static int fail_pipe(int fds[2]) {
    (void)fds;
    const char *ready = getenv("TNY_FIXTURE_PIPE_READY");
    const char *resume = getenv("TNY_FIXTURE_PIPE_RESUME");
    if (!ready || !resume) {
        errno = EMFILE;
        return -1;
    }
    int fd = open(ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) close(fd);
    struct timespec delay = {.tv_nsec = 10000000};
    for (int i = 0; i < 1000 && access(resume, F_OK) != 0; i++) nanosleep(&delay, NULL);
    errno = EMFILE;
    return -1;
}

#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    int (*replacement)(int[2]);
    int (*original)(int[2]);
} pipe_interpose = {fail_pipe, pipe};
#else
int pipe(int fds[2]) { return fail_pipe(fds); }
#endif
#endif
