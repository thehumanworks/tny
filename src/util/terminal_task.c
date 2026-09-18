/* Host OS seam for terminal completion (ADR 0136). Reuses durable-job private
 * files and owner locks, not its scheduler or agent/image submission model. */
#include "util/terminal_task.h"
#include "util/jobs_host.h"
#include "util/util.h"
#include "util/tny_poll.h"
#include "json/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef __EMSCRIPTEN__
#include <sys/file.h>
#include <sys/wait.h>
#endif

bool tny_terminal_supported(void) { return tny_jobs_host_execution_supported(); }

const char *tny_terminal_state_name(tny_terminal_state state) {
    switch (state) {
    case TNY_TERMINAL_STARTING: return "starting";
    case TNY_TERMINAL_RUNNING: return "running";
    case TNY_TERMINAL_COMPLETED: return "completed";
    case TNY_TERMINAL_FAILED: return "failed";
    case TNY_TERMINAL_SIGNALLED: return "signalled";
    case TNY_TERMINAL_LAUNCH_FAILED: return "launch_failed";
    case TNY_TERMINAL_UNKNOWN: return "unknown";
    }
    return "unknown";
}

bool tny_terminal_finished(const tny_terminal_task *task) {
    return task->state != TNY_TERMINAL_STARTING && task->state != TNY_TERMINAL_RUNNING;
}

void tny_terminal_task_free(tny_terminal_task *task) {
    free(task->dir);
    memset(task, 0, sizeof *task);
}

bool tny_terminal_valid_id(const char *id) {
    if (!id || strlen(id) != 16) return false;
    for (int i = 0; i < 16; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

static int task_init(const char *root, const char *id, tny_terminal_task *task) {
    memset(task, 0, sizeof *task);
    task->state = TNY_TERMINAL_UNKNOWN;
    task->exit_code = -1;
    if (!tny_terminal_valid_id(id)) return EINVAL;
    memcpy(task->id, id, sizeof task->id);
    task->dir = path_join(root, id);
    return task->dir ? 0 : ENOMEM;
}

#ifndef __EMSCRIPTEN__
static int publish(const tny_terminal_task *task) {
    char data[192];
    int len =
        snprintf(data, sizeof data, "{\"state\":%d,\"exit_code\":%d,\"signal\":%d,\"error\":%d}\n",
                 (int)task->state, task->exit_code, task->signal, task->error);
    char *path = path_join(task->dir, "status.json");
    int rc = path ? tny_jobs_host_write_private(path, data, (size_t)len) : ENOMEM;
    free(path);
    return rc;
}
#endif

/* Inspectors must not impersonate a live writer to one another. Job admission
 * uses an exclusive probe for conservative reclamation; observation instead
 * takes a shared lock, so only the exclusive launch/waiter lock means held. */
static tny_jobs_owner_state task_owner_state(const char *path) {
#ifdef __EMSCRIPTEN__
    (void)path;
    return TNY_JOBS_OWNER_UNKNOWN;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? TNY_JOBS_OWNER_FREE : TNY_JOBS_OWNER_UNKNOWN;
    struct stat st;
    tny_jobs_owner_state state = TNY_JOBS_OWNER_UNKNOWN;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        if (flock(fd, LOCK_SH | LOCK_NB) == 0) state = TNY_JOBS_OWNER_FREE;
        else if (errno == EWOULDBLOCK) state = TNY_JOBS_OWNER_HELD;
    }
    close(fd);
    return state;
#endif
}

int tny_terminal_inspect(const char *root, const char *id, tny_terminal_task *task) {
    int rc = task_init(root, id, task);
    if (rc) return rc;
    char *lock = path_join(task->dir, "owner.lock");
    char *path = path_join(task->dir, "status.json");
    if (!lock || !path) {
        free(lock);
        free(path);
        return ENOMEM;
    }
    /* Probe before reading: if the lock was released, its final publication
     * precedes this read. Never overwrite the owner's record during inspection. */
    tny_jobs_owner_state owner = task_owner_state(lock);
    char data[1024];
    ssize_t len = -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            st.st_size < (off_t)sizeof data) {
            do { len = read(fd, data, sizeof data); } while (len < 0 && errno == EINTR);
        }
        close(fd);
    }
    yyjson_doc *doc = len > 0 ? jparse(data, (size_t)len) : NULL;
    yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
    int64_t state = jget_int(obj, "state", TNY_TERMINAL_UNKNOWN);
    int64_t code = jget_int(obj, "exit_code", -1);
    int64_t sig = jget_int(obj, "signal", 0);
    if (state >= TNY_TERMINAL_STARTING && state <= TNY_TERMINAL_UNKNOWN && code >= -1 &&
        code <= 255 && sig >= 0 && sig < 128) {
        task->state = (tny_terminal_state)state;
        task->exit_code = (int)code;
        task->signal = (int)sig;
        task->error = (int)jget_int(obj, "error", 0);
    }
    if (!tny_terminal_finished(task) && owner != TNY_JOBS_OWNER_HELD)
        task->state = TNY_TERMINAL_UNKNOWN;
    if ((task->state == TNY_TERMINAL_COMPLETED && task->exit_code != 0) ||
        (task->state == TNY_TERMINAL_FAILED && task->exit_code <= 0) ||
        (task->state == TNY_TERMINAL_SIGNALLED && task->signal == 0))
        task->state = TNY_TERMINAL_UNKNOWN;
    if (task->state != TNY_TERMINAL_COMPLETED && task->state != TNY_TERMINAL_FAILED)
        task->exit_code = -1;
    if (task->state != TNY_TERMINAL_SIGNALLED) task->signal = 0;
    yyjson_doc_free(doc);
    free(lock);
    free(path);
    return 0;
}

#ifndef __EMSCRIPTEN__
/* Keep all handover descriptors clear of stdin/stdout/stderr, including when
 * an embedding caller or shell has closed one of those descriptors. */
static int private_fd(int fd) {
    if (fd < 0) return -1;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    int error = errno;
    close(fd);
    errno = error;
    return copy;
}

static void reset_signals(void) {
    const int signals[] = {SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGPIPE};
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) signal(signals[i], SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

static void command_failure(int fd) {
    int error = errno;
    ssize_t n;
    do { n = write(fd, &error, sizeof error); } while (n < 0 && errno == EINTR);
    _exit(127);
}

static void monitor(tny_terminal_task *task, int owner, int ack, int log, const char *cwd,
                    char *const argv[], void (*setup)(void *), void *ud) {
    int exec_pipe[2];
    if (pipe(exec_pipe) != 0) goto launch_failed;
    if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        goto launch_failed;
    }
    pid_t child = fork();
    if (child < 0) {
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        goto launch_failed;
    }
    if (child == 0) {
        close(owner);
        close(ack);
        close(exec_pipe[0]);
        reset_signals();
        int stdout_fd = dup2(log, STDOUT_FILENO);
        if (stdout_fd < 0) command_failure(exec_pipe[1]);
        int stderr_fd = dup2(log, STDERR_FILENO);
        if (stderr_fd < 0) command_failure(exec_pipe[1]);
        if (log > STDERR_FILENO) close(log);
        if (chdir(cwd) != 0) command_failure(exec_pipe[1]);
        if (setup) setup(ud);
        execv(argv[0], argv);
        command_failure(exec_pipe[1]);
    }
    close(log);
    close(exec_pipe[1]);
    int exec_error = 0;
    ssize_t n;
    do { n = read(exec_pipe[0], &exec_error, sizeof exec_error); } while (n < 0 && errno == EINTR);
    close(exec_pipe[0]);
    task->state = n == 0 ? TNY_TERMINAL_RUNNING : TNY_TERMINAL_LAUNCH_FAILED;
    task->error = n == sizeof exec_error ? exec_error : (n == 0 ? 0 : EIO);
    int published = publish(task);
    char accepted = published == 0 ? '1' : '0';
    /* A lost caller does not cancel accepted work. SIGPIPE is ignored here. */
    do { n = write(ack, &accepted, 1); } while (n < 0 && errno == EINTR);
    close(ack);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        task->state = TNY_TERMINAL_UNKNOWN;
    } else if (task->state != TNY_TERMINAL_LAUNCH_FAILED) {
        if (WIFEXITED(status)) {
            task->exit_code = WEXITSTATUS(status);
            task->state = task->exit_code == 0 ? TNY_TERMINAL_COMPLETED : TNY_TERMINAL_FAILED;
        } else if (WIFSIGNALED(status)) {
            task->signal = WTERMSIG(status);
            task->state = TNY_TERMINAL_SIGNALLED;
        } else task->state = TNY_TERMINAL_UNKNOWN;
    }
    (void)publish(task);
    close(owner);
    _exit(0);

launch_failed:
    task->error = errno;
    task->state = TNY_TERMINAL_LAUNCH_FAILED;
    (void)publish(task);
    close(owner);
    close(ack);
    _exit(1);
}
#endif

int tny_terminal_start(const char *root, const char *cwd, char *const argv[], void (*setup)(void *),
                       void *ud, tny_terminal_task *task) {
    memset(task, 0, sizeof *task);
    task->state = TNY_TERMINAL_UNKNOWN;
    task->exit_code = -1;
#ifdef __EMSCRIPTEN__
    (void)root;
    (void)cwd;
    (void)argv;
    (void)setup;
    (void)ud;
    return ENOTSUP;
#else
    if (!tny_terminal_supported()) return ENOTSUP;
    if (!root || !cwd || !argv || !argv[0]) return EINVAL;
    char *id = gen_id();
    int rc = task_init(root, id, task);
    free(id);
    if (rc) return rc;
    rc = tny_jobs_host_mkdir_private(root);
    if (!rc && mkdir(task->dir, 0700) != 0) rc = errno;
    if (rc) {
        tny_terminal_task_free(task);
        return rc;
    }
    char *lockpath = path_join(task->dir, "owner.lock");
    char *logpath = path_join(task->dir, "output.log");
    int owner = lockpath ? private_fd(tny_jobs_host_lock_open(lockpath)) : -1;
    int log =
        logpath ? private_fd(open(logpath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) : -1;
    free(lockpath);
    free(logpath);
    int ack[2] = {-1, -1};
    bool transferred = false;
    if (owner < 0 || log < 0 || tny_jobs_host_lock_try(owner) != TNY_JOBS_LOCK_ACQUIRED) {
        rc = EIO;
        goto done;
    }
    task->state = TNY_TERMINAL_STARTING;
    rc = publish(task);
    if (rc) goto done;
    if (pipe(ack) != 0) {
        rc = errno;
        goto done;
    }
    ack[0] = private_fd(ack[0]);
    ack[1] = private_fd(ack[1]);
    if (ack[0] < 0 || ack[1] < 0) {
        rc = EIO;
        goto done;
    }
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > INT_MAX) {
        rc = EIO;
        goto done;
    }
    pid_t launcher = fork();
    if (launcher < 0) {
        rc = errno;
        goto done;
    }
    if (launcher == 0) {
        reset_signals();
        if (setsid() < 0) _exit(1);
        pid_t waiter = fork();
        if (waiter < 0) _exit(1);
        if (waiter > 0) _exit(0);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        /* Do not retain runner sockets, provider pipes or session locks. */
        for (int fd = 0; fd < maxfd; fd++)
            if (fd != owner && fd != ack[1] && fd != log) close(fd);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd < 0 || dup2(nullfd, 0) < 0 || dup2(nullfd, 1) < 0 || dup2(nullfd, 2) < 0)
            _exit(1);
        if (nullfd > 2) close(nullfd);
        monitor(task, owner, ack[1], log, cwd, argv, setup, ud);
    }
    transferred = true;
    close(ack[1]);
    ack[1] = -1;
    close(owner);
    owner = -1;
    close(log);
    log = -1;
    int status = 0;
    pid_t waited;
    do { waited = waitpid(launcher, &status, 0); } while (waited < 0 && errno == EINTR);
    /* Only the launch child belongs to us. The detached waiter owns command
     * status; neither this path nor inspection can reap a provider child. */
    if (waited != launcher || !WIFEXITED(status) || WEXITSTATUS(status) != 0) rc = EIO;
    else {
        struct pollfd pf = {ack[0], POLLIN, 0};
        int64_t deadline = monotonic_ms() + 5000;
        int pr = 0;
        for (;;) {
            int64_t left = deadline - monotonic_ms();
            if (left <= 0) break;
            pr = tny_poll(&pf, 1, (int)left);
            if (pr >= 0 || errno != EINTR) break;
        }
        char accepted = 0;
        rc = pr > 0 && read(ack[0], &accepted, 1) == 1 && accepted == '1' ? 0 : EIO;
    }
done:
    if (rc && !transferred) {
        task->state = TNY_TERMINAL_LAUNCH_FAILED;
        task->error = rc;
        (void)publish(task);
    }
    if (owner >= 0) close(owner);
    if (log >= 0) close(log);
    if (ack[0] >= 0) close(ack[0]);
    if (ack[1] >= 0) close(ack[1]);
    /* Once a waiter exists it is the sole writer. A failed acknowledgment is
     * not proof of launch failure and must never overwrite its result. */
    return rc;
#endif
}
