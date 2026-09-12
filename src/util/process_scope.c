/* Launch-time ownership for durable jobs, within the host OS seam (ADR 0099). */
#include "util/process.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/stat.h>
#include <sys/wait.h>
#endif
#if defined(__MSYS__) || defined(__CYGWIN__)
#include <windows.h>
#include <sys/cygwin.h>
#define SCOPE_WINDOWS 1
#else
#define SCOPE_WINDOWS 0
#endif

#define SCOPE_VERSION    TNY_JOB_SCOPE_PREFIX "VERSION"
#define SCOPE_NAME       TNY_JOB_SCOPE_PREFIX "NAME"
#define SCOPE_ACK_FD     TNY_JOB_SCOPE_PREFIX "ACK_FD"
#define SCOPE_JOB_PREFIX "Local\\tny-job-"
#define SCOPE_NAME_SIZE  (sizeof SCOPE_JOB_PREFIX + 32)

extern char **environ;

struct tny_process_scope {
    pid_t pid;
    bool reaped, root_unknown, ack_ready, released, stopping;
    int status, ack_fd;
    size_t ack_len;
    char ack[4];
    int64_t deadline;
    bool cleanup_started, cleanup_error, cleanup_proven, cleanup_forced;
    int64_t cleanup_deadline;
#if SCOPE_WINDOWS
    HANDLE job;
    struct scope_observation {
        HANDLE process; /* synchronization/query only; never termination authority */
        pid_t pid;
    } *observed;
    size_t observed_count;
#endif
};

#if SCOPE_WINDOWS
/* Never closed while this supervisor continues running, nor inherited. */
static HANDLE supervisor_job;
static pid_t supervisor_pid;

static HANDLE scope_job_create(const char *name) {
    HANDLE job = CreateJobObjectA(NULL, name);
    if (!job) return NULL;
    if (name && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(job);
        return NULL;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}
#endif

bool tny_process_scope_native_jobs(void) { return SCOPE_WINDOWS != 0; }

bool tny_process_scope_env_reserved(const char *entry) {
    return entry && strncmp(entry, TNY_JOB_SCOPE_PREFIX, sizeof TNY_JOB_SCOPE_PREFIX - 1) == 0;
}

int tny_process_supervisor_init(void) {
#ifdef __EMSCRIPTEN__
    return ENOTSUP;
#else
    struct sigaction action = {0};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL) != 0) return errno;
#if SCOPE_WINDOWS
    if (supervisor_job) return supervisor_pid == getpid() ? 0 : EINVAL;
    HANDLE job = scope_job_create(NULL);
    if (!job) return EIO;
    if (!AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return EIO;
    }
    supervisor_job = job;
    supervisor_pid = getpid();
#endif
    return 0;
#endif
}

/* Take all three fields together; duplicate and unknown reserved fields fail.
 * Clear even malformed tuples before reporting a bounded, secret-free error. */
static int admission_fields(char name[SCOPE_NAME_SIZE]) {
    const char *keys[] = {SCOPE_VERSION, SCOPE_NAME, SCOPE_ACK_FD};
    const char *values[3] = {NULL, NULL, NULL};
    unsigned seen = 0;
    bool invalid = false;
    for (size_t i = 0; environ && environ[i]; i++) {
        const char *entry = environ[i];
        if (!tny_process_scope_env_reserved(entry)) continue;
        const char *equal = strchr(entry, '=');
        int index = -1;
        for (int k = 0; equal && k < 3; k++)
            if ((size_t)(equal - entry) == strlen(keys[k]) &&
                strncmp(entry, keys[k], (size_t)(equal - entry)) == 0)
                index = k;
        if (index < 0 || (seen & (1u << index))) invalid = true;
        else {
            seen |= 1u << index;
            values[index] = equal + 1;
        }
    }
    bool any = seen || invalid;
    if (any) {
        if (seen != 7 || strcmp(values[0] ? values[0] : "", "1") != 0 ||
            strcmp(values[2] ? values[2] : "", "3") != 0 || !values[1] ||
            strlen(values[1]) != SCOPE_NAME_SIZE - 1 ||
            strncmp(values[1], SCOPE_JOB_PREFIX, sizeof SCOPE_JOB_PREFIX - 1) != 0)
            invalid = true;
        if (!invalid) {
            memcpy(name, values[1], SCOPE_NAME_SIZE);
            for (size_t i = sizeof SCOPE_JOB_PREFIX - 1; i < SCOPE_NAME_SIZE - 1; i++)
                if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
                    invalid = true;
        }
        /* unsetenv changes environ; repeatedly locate the first reserved name. */
        for (size_t i = 0; environ && environ[i];) {
            if (!tny_process_scope_env_reserved(environ[i])) {
                i++;
                continue;
            }
            const char *equal = strchr(environ[i], '=');
            if (!equal) return -EINVAL;
            size_t len = (size_t)(equal - environ[i]);
            char *key = malloc(len + 1);
            if (!key) return -ENOMEM;
            memcpy(key, environ[i], len);
            key[len] = 0;
            int rc = unsetenv(key);
            free(key);
            if (rc != 0) return -EIO;
            i = 0;
        }
    }
    return invalid ? -EINVAL : any ? 1 : 0;
}

int tny_process_scope_admit(void) {
    char name[SCOPE_NAME_SIZE] = {0};
    int fields = admission_fields(name);
    if (fields <= 0) return -fields;
#if SCOPE_WINDOWS
    HANDLE job = OpenJobObjectA(JOB_OBJECT_ASSIGN_PROCESS, FALSE, name);
    if (!job) return EIO;
    bool admitted = AssignProcessToJobObject(job, GetCurrentProcess()) != 0;
    CloseHandle(job);
    if (!admitted) return EIO;
    int flags = fcntl(3, F_GETFL);
    if (flags < 0 || fcntl(3, F_SETFL, flags | O_NONBLOCK) != 0) return EIO;
    ssize_t written;
    do written = write(3, "ok\n", 3);
    while (written < 0 && errno == EINTR);
    close(3);
    if (written != 3) return EIO;
    int64_t deadline = monotonic_ms() + TNY_JOB_SCOPE_TIMEOUT_MS;
    for (;;) {
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) return ETIMEDOUT;
        struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
        int ready = tny_poll(&fd, 1, left > 100 ? 100 : (int)left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return EIO;
        if (!ready) continue;
        char go;
        ssize_t n = read(STDIN_FILENO, &go, 1);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return n == 1 && go == 'G' ? 0 : EIO;
    }
#else
    return ENOTSUP;
#endif
}

#if SCOPE_WINDOWS
static int scope_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}
#endif

int tny_process_scope_spawn(char *const argv[], char *const envp[], int in_fd, int out_fd,
                            tny_process_scope **out) {
    if (!out) return EINVAL;
    *out = NULL;
#ifdef __EMSCRIPTEN__
    (void)argv;
    (void)envp;
    (void)in_fd;
    (void)out_fd;
    return ENOTSUP;
#else
    if (!argv || !argv[0] || !envp) return EINVAL;
    char *self = tny_process_self_path();
    struct stat own, requested;
    bool trusted = self && stat(self, &own) == 0 && stat(argv[0], &requested) == 0 &&
                   own.st_dev == requested.st_dev && own.st_ino == requested.st_ino;
    free(self);
    if (!trusted) return EINVAL;
    tny_process_scope *scope = calloc(1, sizeof *scope);
    if (!scope) return ENOMEM;
    scope->ack_fd = -1;
    int rc = 0, ack[2] = {-1, -1};
    size_t count = 0, used = 0;
    while (envp[count]) count++;
    char **child_env = calloc(count + 4, sizeof *child_env);
    if (!child_env) {
        free(scope);
        return ENOMEM;
    }
    for (size_t i = 0; i < count; i++)
        if (!tny_process_scope_env_reserved(envp[i])) child_env[used++] = envp[i];
    tny_fd_mapping maps[3] = {{in_fd, 0}, {out_fd, 1}, {-1, 3}};
    int n_maps = 2;
#if SCOPE_WINDOWS
    char name[SCOPE_NAME_SIZE] = SCOPE_JOB_PREFIX;
    char name_env[sizeof SCOPE_NAME + SCOPE_NAME_SIZE];
    if (!supervisor_job || supervisor_pid != getpid()) rc = EINVAL;
    uint8_t nonce[16];
    if (!rc && !random_bytes(nonce, sizeof nonce)) rc = EIO;
    if (!rc) {
        for (size_t i = 0; i < sizeof nonce; i++)
            snprintf(name + sizeof SCOPE_JOB_PREFIX - 1 + 2 * i, 3, "%02x", nonce[i]);
        scope->job = scope_job_create(name);
        if (!scope->job) rc = EIO;
    }
    if (!rc && pipe(ack) != 0) rc = errno;
    if (!rc && (scope_cloexec(ack[0]) != 0 || scope_cloexec(ack[1]) != 0 ||
                fcntl(ack[0], F_SETFL, O_NONBLOCK) < 0))
        rc = errno;
    if (!rc) {
        snprintf(name_env, sizeof name_env, "%s=%s", SCOPE_NAME, name);
        child_env[used++] = (char *)SCOPE_VERSION "=1";
        child_env[used++] = name_env;
        child_env[used++] = (char *)SCOPE_ACK_FD "=3";
        maps[2].source = ack[1];
        n_maps = 3;
    }
#else
    scope->ack_ready = true;
#endif
    if (!rc) rc = tny_process_spawn_mapped(argv, child_env, maps, n_maps, &scope->pid);
    free(child_env);
    if (ack[1] >= 0) close(ack[1]);
    if (rc) {
        if (ack[0] >= 0) close(ack[0]);
#if SCOPE_WINDOWS
        if (scope->job) CloseHandle(scope->job);
#endif
        free(scope);
        return rc;
    }
    scope->ack_fd = ack[0];
    scope->deadline = monotonic_ms() + TNY_JOB_SCOPE_TIMEOUT_MS;
    *out = scope;
    return 0;
#endif
}

pid_t tny_process_scope_pid(const tny_process_scope *scope) { return scope ? scope->pid : -1; }

int tny_process_scope_ack(tny_process_scope *scope) {
    if (!scope) return -1;
    if (scope->ack_ready) return 1;
    if (scope->ack_fd < 0 || monotonic_ms() >= scope->deadline) return -1;
    ssize_t n =
        read(scope->ack_fd, scope->ack + scope->ack_len, sizeof scope->ack - scope->ack_len);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n < 0) return -1;
    if (n > 0) {
        scope->ack_len += (size_t)n;
        return scope->ack_len == sizeof scope->ack ? -1 : 0;
    }
    close(scope->ack_fd);
    scope->ack_fd = -1;
    scope->ack_ready = scope->ack_len == 3 && memcmp(scope->ack, "ok\n", 3) == 0;
    return scope->ack_ready ? 1 : -1;
}

int tny_process_scope_go(tny_process_scope *scope, int prompt_fd) {
    if (!scope || !scope->ack_ready || scope->reaped || scope->root_unknown || scope->stopping)
        return -1;
    if (scope->released) return 1;
#if SCOPE_WINDOWS
    int flags = fcntl(prompt_fd, F_GETFL);
    if (flags < 0 || fcntl(prompt_fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    ssize_t n = write(prompt_fd, "G", 1);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n != 1) return -1;
#else
    (void)prompt_fd;
#endif
    scope->released = true;
    return 1;
}

int tny_process_scope_reap(tny_process_scope *scope, int *status) {
    /* Ownership loss is terminal: this number can belong to a later child. */
    if (!scope || scope->root_unknown) return -1;
#ifndef __EMSCRIPTEN__
    if (!scope->reaped) {
        pid_t got = waitpid(scope->pid, &scope->status, WNOHANG);
        if (!got || (got < 0 && errno == EINTR)) return 0;
        if (got != scope->pid) {
            scope->root_unknown = true;
            return -1;
        }
        scope->reaped = true;
    }
    if (status) *status = scope->status;
    return 1;
#else
    (void)status;
    return -1;
#endif
}

int tny_process_scope_terminate(tny_process_scope *scope) {
    if (!scope) return EINVAL;
    scope->stopping = true;
#if SCOPE_WINDOWS
    int rc = TerminateJobObject(scope->job, 130) ? 0 : EIO;
    if (!scope->released && !scope->reaped && !scope->root_unknown &&
        kill(scope->pid, SIGKILL) != 0 && errno != ESRCH)
        rc = errno;
    return rc;
#elif !defined(__EMSCRIPTEN__)
    if (scope->root_unknown) return ECHILD;
    if (scope->reaped) return 0;
    bool reaped = false;
    int rc = tny_process_stop_owned_tree(scope->pid, &scope->status, &reaped);
    scope->reaped = reaped;
    /* The delegated sweep consumes waits too. A failed/unobserved reap cannot
     * retain PID authority merely because this wrapper did not call waitpid. */
    if (!reaped) scope->root_unknown = true;
    return rc == 0 && reaped ? 0 : EIO;
#else
    return ENOTSUP;
#endif
}

int tny_process_scope_empty(const tny_process_scope *scope) {
    if (!scope) return -1;
#if SCOPE_WINDOWS
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {0};
    if (!QueryInformationJobObject(scope->job, JobObjectBasicAccountingInformation, &info,
                                   sizeof info, NULL))
        return -1;
    return info.ActiveProcesses == 0 ? 1 : 0;
#else
    return scope->reaped ? 1 : 0;
#endif
}

#if SCOPE_WINDOWS
/* A complete bounded kernel snapshot, followed by identity-bound observations.
 * Numeric ids are used only to open query handles and to check strict absence. */
static int scope_capture(tny_process_scope *scope) {
    enum { MAX_MEMBERS = 4096 };
    JOBOBJECT_BASIC_PROCESS_ID_LIST *list = NULL;
    size_t capacity = 32;
    bool complete = false;
    for (int attempt = 0; attempt < 8 && monotonic_ms() < scope->cleanup_deadline; attempt++) {
        size_t bytes =
            offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) + capacity * sizeof(ULONG_PTR);
        free(list);
        list = calloc(1, bytes);
        if (!list) return -1;
        BOOL ok = QueryInformationJobObject(scope->job, JobObjectBasicProcessIdList, list,
                                            (DWORD)bytes, NULL);
        if (ok && list->NumberOfAssignedProcesses == list->NumberOfProcessIdsInList &&
            list->NumberOfProcessIdsInList <= capacity) {
            complete = true;
            break;
        }
        if ((!ok && GetLastError() != ERROR_MORE_DATA) || capacity == MAX_MEMBERS) break;
        size_t next = capacity * 2;
        if (list->NumberOfAssignedProcesses > next) next = list->NumberOfAssignedProcesses;
        capacity = next > MAX_MEMBERS ? MAX_MEMBERS : next;
    }
    if (!complete) {
        free(list);
        return -1;
    }
    scope->observed_count = list->NumberOfProcessIdsInList;
    if (scope->observed_count) {
        scope->observed = calloc(scope->observed_count, sizeof *scope->observed);
        if (!scope->observed) {
            scope->observed_count = 0;
            free(list);
            return -1;
        }
    }
    int rc = 0;
    for (size_t i = 0; i < scope->observed_count; i++) {
        ULONG_PTR number = list->ProcessIdList[i];
        if (number > UINT32_MAX || number <= 1) {
            rc = -1;
            continue;
        }
        HANDLE process =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)number);
        BOOL member = FALSE;
        if (!process || !IsProcessInJob(process, scope->job, &member) || !member) {
            if (process) CloseHandle(process);
            rc = -1; /* exited/reused/unqueryable is uncertainty, never new authority */
            continue;
        }
        scope->observed[i].process = process;
        /* MSYS maps a native-only member to its explicit WinPID+MAX_PID alias.
         * Both real POSIX ids and these aliases support read-only kill(pid,0).
         * Never substitute a guessed Windows id when this API fails. */
        uintptr_t mapped = cygwin_internal(CW_WINPID_TO_CYGWIN_PID, (DWORD)number);
        if (mapped <= 1 || mapped > INT_MAX) rc = -1;
        else scope->observed[i].pid = (pid_t)mapped;
    }
    free(list);
    return rc;
}
#endif

int tny_process_scope_cleanup(tny_process_scope *scope, bool force, bool *forced) {
    if (!scope) return -1;
    if (forced) *forced = scope->cleanup_forced;
    if (scope->cleanup_proven) return 1;
    if (!scope->cleanup_started) {
        if (!force && !scope->reaped && !scope->root_unknown) return 0;
        scope->cleanup_started = true;
        scope->cleanup_deadline = monotonic_ms() + 3000;
#if SCOPE_WINDOWS
        int empty = tny_process_scope_empty(scope);
        if (empty < 0 || scope_capture(scope) != 0) scope->cleanup_error = true;
        if (force || empty != 1) {
            scope->cleanup_forced = true;
            if (tny_process_scope_terminate(scope) != 0) scope->cleanup_error = true;
        }
#else
        if (force) {
            scope->cleanup_forced = true;
            if (tny_process_scope_terminate(scope) != 0) scope->cleanup_error = true;
        }
#endif
    }
    if (forced) *forced = scope->cleanup_forced;
    int reaped = tny_process_scope_reap(scope, NULL);
    if (reaped < 0) scope->cleanup_error = true;
    bool observed_dead = true;
#if SCOPE_WINDOWS
    for (size_t i = 0; i < scope->observed_count; i++) {
        if (!scope->observed[i].process) continue;
        DWORD state = WaitForSingleObject(scope->observed[i].process, 0);
        if (state == WAIT_OBJECT_0) {
            CloseHandle(scope->observed[i].process);
            scope->observed[i].process = NULL;
        } else {
            observed_dead = false;
            if (state != WAIT_TIMEOUT) scope->cleanup_error = true;
        }
    }
#endif
    bool absent = reaped == 1 && kill(scope->pid, 0) < 0 && errno == ESRCH;
#if SCOPE_WINDOWS
    for (size_t i = 0; absent && i < scope->observed_count; i++) {
        pid_t pid = scope->observed[i].pid;
        if (pid <= 1 || kill(pid, 0) == 0 || errno != ESRCH) absent = false;
    }
#endif
    int empty = tny_process_scope_empty(scope);
    if (empty < 0) scope->cleanup_error = true;
    if (reaped == 1 && observed_dead && absent && empty == 1) {
        if (scope->cleanup_error) return -1;
        scope->cleanup_proven = true;
        return 1;
    }
    if (monotonic_ms() >= scope->cleanup_deadline) return -1;
    return 0;
}

int tny_process_scope_destroy(tny_process_scope *scope) {
    if (!scope) return 0;
    if (scope->root_unknown || !scope->reaped || tny_process_scope_empty(scope) != 1) return EBUSY;
    if (scope->ack_fd >= 0) close(scope->ack_fd);
#if SCOPE_WINDOWS
    for (size_t i = 0; i < scope->observed_count; i++)
        if (scope->observed[i].process) CloseHandle(scope->observed[i].process);
    free(scope->observed);
    CloseHandle(scope->job);
#endif
    free(scope);
    return 0;
}
