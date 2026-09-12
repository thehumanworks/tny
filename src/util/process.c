#include "util/process.h"

#include "util/util.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <spawn.h>
#include <sys/wait.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#elif defined(__linux__)
#include <dirent.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
/* Freeze each parent before enumerating its children; children are then
 * frozen before their own enumeration. This also keeps ordinary pid reuse
 * out of the subsequent kill sweep. No shell or external ps is involved. */
static int child_pids(pid_t parent, pid_t *out, int cap) {
    if (cap <= 0) return -1;
#if defined(__APPLE__)
    int n = proc_listchildpids(parent, out, cap * (int)sizeof *out);
    return n >= cap ? -1 : n;
#else
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 1) continue;
        char path[64], stat[1024];
        snprintf(path, sizeof path, "/proc/%ld/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue; /* exited during enumeration */
        bool read_ok = fgets(stat, sizeof stat, f) != NULL;
        fclose(f);
        if (!read_ok) continue;
        /* comm is parenthesized and may itself contain spaces or ')'. */
        char *close = strrchr(stat, ')');
        long ppid = 0;
        if (!close || sscanf(close + 1, " %*c %ld", &ppid) != 1 || ppid != parent) continue;
        if (n == cap) {
            n = -1;
            break;
        }
        out[n++] = (pid_t)pid;
    }
    closedir(dir);
    return n;
#endif
}
#endif

int tny_process_kill_tree(pid_t root) {
#ifdef __EMSCRIPTEN__
    (void)root;
    errno = ENOTSUP;
    return -1;
#else
    if (root <= 1 || root == getpid() || root == getpgrp()) {
        errno = EINVAL;
        return -1;
    }
    int rc = 0;
#if defined(__APPLE__) || defined(__linux__)
    enum { MAX_PROCESSES = 4096 };
    pid_t *pids = calloc(MAX_PROCESSES, sizeof *pids);
    if (!pids) rc = -1;
    else {
        int count = 1;
        pids[0] = root;
        for (int i = 0; i < count; i++) {
            if (pids[i] <= 1 || pids[i] == getpid()) {
                rc = -1;
                continue;
            }
            if (kill(pids[i], SIGSTOP) != 0) {
                if (errno != ESRCH) rc = -1;
                continue;
            }
            int n = child_pids(pids[i], pids + count, MAX_PROCESSES - count);
            if (n < 0) rc = -1;
            else count += n;
        }
        /* Children first, while their stopped parents still pin identity.
         * A group's leader may have forked wrappers; sweep its group too. */
        for (int i = count - 1; i > 0; i--) {
            if (pids[i] <= 1 || pids[i] == getpid()) continue;
            if (getpgid(pids[i]) == pids[i]) kill(-pids[i], SIGKILL);
            if (kill(pids[i], SIGKILL) != 0 && errno != ESRCH) rc = -1;
        }
        free(pids);
    }
#endif
    if (kill(-root, SIGKILL) != 0 && errno != ESRCH) rc = -1;
    if (kill(root, SIGKILL) != 0 && errno != ESRCH) rc = -1;
    return rc;
#endif
}

/* States are observed while the caller retains its unreaped root and each
 * descendant's parent is stopped. A parent is not killed until its children
 * have stopped executing. No process-group sweep or post-reap PID signalling. */
#if defined(__APPLE__) || defined(__linux__)
/* Every signal below carries a kernel-checked process generation. */

typedef struct {
    int fd;
#ifdef __APPLE__
    uint64_t unique;
    audit_token_t token;
#endif
} tree_ref;

#ifdef __APPLE__
/* proc_pidinfo flavor 18 ABI, pinned to Apple xnu-12377.81.4
 * bsd/sys/proc_info_private.h. Public SDKs omit the flavor's structure.
 * Fetch parent identity and PID generation in ONE kernel snapshot. */
struct tree_darwin_identity {
    struct proc_bsdinfo bsd;
    struct {
        uint8_t uuid[16];
        uint64_t unique, parent_unique;
        int32_t version;
        uint32_t reserved1;
        uint64_t reserved2, reserved3;
    } identity;
};
extern int proc_signal_with_audittoken(audit_token_t *, int) __attribute__((weak_import));
#endif

static tree_ref tree_handle(pid_t pid, pid_t parent) {
    tree_ref ref = {.fd = -1};
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    (void)parent;
    ref.fd = (int)syscall(SYS_pidfd_open, pid, 0);
#elif defined(__APPLE__)
    struct tree_darwin_identity info;
    if (!proc_signal_with_audittoken) {
        errno = ENOTSUP;
        return ref;
    }
    int n = proc_pidinfo(pid, 18, 0, &info, sizeof info);
    if (n != sizeof info || info.bsd.pbi_ppid != (uint32_t)parent) return ref;
    ref.unique = info.identity.unique;
    ref.token.val[1] = info.bsd.pbi_uid;
    ref.token.val[2] = info.bsd.pbi_gid;
    ref.token.val[3] = info.bsd.pbi_ruid;
    ref.token.val[4] = info.bsd.pbi_rgid;
    ref.token.val[5] = (uint32_t)pid;
    ref.token.val[7] = (uint32_t)info.identity.version;
    ref.fd = 0;
#else
    (void)pid;
    (void)parent;
    errno = ENOTSUP;
#endif
    return ref;
}

static int tree_signal(pid_t pid, tree_ref *ref, int sig) {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
    (void)pid;
    return (int)syscall(SYS_pidfd_send_signal, ref->fd, sig, NULL, 0);
#elif defined(__APPLE__)
    int rc = ESRCH;
    for (int attempt = 0; attempt < 8; attempt++) {
        rc = proc_signal_with_audittoken(&ref->token, sig);
        if (rc != ESRCH) break;
        /* exec changes pidversion, but not the process unique ID. Refresh
         * only that same previously owned process, never a recycled PID. */
        struct tree_darwin_identity info;
        if (proc_pidinfo(pid, 18, 0, &info, sizeof info) != sizeof info ||
            info.identity.unique != ref->unique)
            break;
        ref->token.val[7] = (uint32_t)info.identity.version;
    }
    if (rc) errno = rc;
    return rc ? -1 : 0;
#else
    (void)pid;
    (void)ref;
    (void)sig;
    errno = ENOTSUP;
    return -1;
#endif
}

static int owned_state(pid_t pid, pid_t parent) {
#if defined(__APPLE__)
    /* proc_pidinfo omits zombies on Darwin. KERN_PROC_PID observes them while
     * the stopped parent still pins identity, without reaping the child. */
    struct kinfo_proc info;
    size_t size = sizeof info;
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) return -1;
    if (!size) return 3;
    if (size != sizeof info || info.kp_eproc.e_ppid != parent) return -1;
    if (info.kp_proc.p_stat == SZOMB) return 2;
    return info.kp_proc.p_stat == SSTOP ? 1 : 0;
#else
    char path[64], data[4096], state;
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT ? 3 : -1;
    bool ok = fgets(data, sizeof data, file) != NULL;
    fclose(file);
    char *end = ok ? strrchr(data, ')') : NULL;
    long ppid = 0;
    if (!end || sscanf(end + 1, " %c %ld", &state, &ppid) != 2 || ppid != parent) return -1;
    if (state == 'Z' || state == 'X') return 2;
    return state == 'T' || state == 't' ? 1 : 0;
#endif
}
#endif

bool tny_process_tree_supported(void) {
#if defined(__APPLE__) || defined(__linux__)
    tree_ref ref = tree_handle(getpid(), getppid());
    bool ok = ref.fd >= 0;
#if defined(__linux__)
    if (ok) close(ref.fd);
#endif
    return ok;
#else
    return false;
#endif
}

int tny_process_stop_owned_tree(pid_t root, int *status, bool *reaped) {
    if (reaped) *reaped = false;
#if defined(__APPLE__) || defined(__linux__)
    enum { CAP = 4096 };
    struct node {
        pid_t pid, parent;
        tree_ref handle;
        bool dead;
    };
    struct node *nodes = calloc(CAP, sizeof *nodes);
    pid_t *children = calloc(CAP, sizeof *children);
    if (!nodes || !children) {
        free(nodes);
        free(children);
        return -1;
    }
    int count = 1, rc = 0;
    nodes[0].pid = root;
    nodes[0].parent = getpid();
    nodes[0].handle.fd = -1;
    int64_t deadline = monotonic_ms() + 3000;
    for (int i = 0; i < count; i++) {
        struct node *node = &nodes[i];
        node->handle = tree_handle(node->pid, node->parent);
        int state = owned_state(node->pid, node->parent);
        if (node->handle.fd < 0 || state < 0) {
            /* A generation handle alone does not establish ownership. Never
             * retain authority to signal a process whose first parent check
             * failed, including in the final cleanup sweep. */
#if defined(__linux__)
            if (node->handle.fd >= 0) close(node->handle.fd);
#endif
            node->handle.fd = -1;
            rc = -1;
            continue;
        }
        if (state >= 2) {
            node->dead = true;
            rc = -1; /* descendants may have escaped before capture */
            continue;
        }
        if (tree_signal(node->pid, &node->handle, SIGSTOP) != 0) {
            rc = -1;
            continue;
        }
        while ((state = owned_state(node->pid, node->parent)) == 0 && monotonic_ms() < deadline)
            usleep(1000);
        if (state != 1) {
            node->dead = state >= 2;
            rc = -1;
            continue;
        }
        /* The parent is now frozen; enumerate its children. */
        int n = child_pids(node->pid, children, CAP - count);
        if (n < 0) {
            rc = -1;
            continue;
        }
        for (int k = 0; k < n; k++) {
            nodes[count].pid = children[k];
            nodes[count].handle.fd = -1;
            nodes[count++].parent = node->pid;
        }
    }
    for (int i = count - 1; i >= 0; i--) {
        struct node *node = &nodes[i];
        if (node->dead) continue;
        /* Even a failed freeze must not abandon a previously proven-owned
         * generation. Only after that initial check may the immutable handle
         * survive auto-reaping or ancestry loss. Never use a raw PID/group. */
        if (node->handle.fd < 0) {
            rc = -1;
            continue;
        }
        if (tree_signal(node->pid, &node->handle, SIGKILL) != 0 && errno != ESRCH) rc = -1;
    }
    /* Reap the actual direct child before checking strict PID absence. Its
     * children are adopted/reaped by the host reaper after parents die. No
     * further signals are sent during this phase, even if a PID is reused. */
    int child_status = 0;
    pid_t got;
    int64_t reap_deadline = monotonic_ms() + 5000;
    do {
        got = waitpid(root, &child_status, WNOHANG);
        if (got < 0 && errno == EINTR) continue;
        if (got != 0) break;
        (void)tny_poll(NULL, 0, 10);
    } while (monotonic_ms() < reap_deadline);
    if (got == root) {
        if (status) *status = child_status;
        if (reaped) *reaped = true;
    } else rc = -1;
    bool absent = false;
    do {
        absent = true;
        for (int i = 0; i < count; i++) {
            if (kill(nodes[i].pid, 0) == 0 || errno != ESRCH) {
                absent = false;
                break;
            }
        }
        if (!absent) (void)tny_poll(NULL, 0, 10);
    } while (!absent && monotonic_ms() < reap_deadline);
    if (!absent) rc = -1;
#if defined(__linux__)
    for (int i = 0; i < count; i++)
        if (nodes[i].handle.fd >= 0) close(nodes[i].handle.fd);
#endif
    free(children);
    free(nodes);
    return rc;
#else
    (void)root;
    (void)status;
    errno = ENOTSUP;
    return -1;
#endif
}

char *tny_process_self_path(void) {
#ifdef __EMSCRIPTEN__
    errno = ENOTSUP;
    return NULL;
#else
    char buf[4096];
#if defined(__APPLE__)
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) != 0) return NULL;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return NULL;
    if ((size_t)n == sizeof buf - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    buf[n] = 0;
#endif
    char *abs = path_abs(buf);
    if (abs && abs[0] == '/') return abs;
    free(abs);
    errno = ENOENT;
    return NULL;
#endif
}

int tny_process_spawn(char *const argv[], char *const envp[], int in_fd, int out_fd, pid_t *pid) {
    const tny_fd_mapping maps[] = {{in_fd, STDIN_FILENO}, {out_fd, STDOUT_FILENO}};
    return tny_process_spawn_mapped(argv, envp, maps, 2, pid);
}

#ifndef __EMSCRIPTEN__
/* Stage every source descriptor above every target and every other source, so
 * adding the dup2 actions below can never overwrite a source that has not been
 * copied yet. F_DUPFD_CLOEXEC always returns the lowest free descriptor at or
 * above `base`, and each staged copy stays open, so the copies cannot collide
 * with each other either. */
static int stage_sources(const tny_fd_mapping *maps, int n_maps, int *staged) {
    /* Every slot is a definite value before anything can read one back, so a
     * partial failure closes exactly what was staged and nothing else. */
    for (int i = 0; i < n_maps; i++) staged[i] = -1;
    int base = 10;
    for (int i = 0; i < n_maps; i++) {
        if (maps[i].source >= base) base = maps[i].source + 1;
        if (maps[i].target >= base) base = maps[i].target + 1;
    }
    for (int i = 0; i < n_maps; i++) {
        staged[i] = fcntl(maps[i].source, F_DUPFD_CLOEXEC, base);
        if (staged[i] < 0) {
            int e = errno;
            for (int j = 0; j < i; j++) close(staged[j]);
            return e;
        }
    }
    return 0;
}
#endif

int tny_process_spawn_mapped(char *const argv[], char *const envp[], const tny_fd_mapping *maps,
                             int n_maps, pid_t *pid) {
#ifdef __EMSCRIPTEN__
    (void)argv;
    (void)envp;
    (void)maps;
    (void)n_maps;
    (void)pid;
    return ENOTSUP;
#else
    if (!argv || !argv[0] || argv[0][0] != '/' || !envp || !pid) return EINVAL;
    if (!maps || n_maps < 1 || n_maps > TNY_PROCESS_MAX_FD_MAPPINGS) return EINVAL;
    for (int i = 0; i < n_maps; i++) {
        if (maps[i].source < 0 || maps[i].target < 0 || maps[i].target >= 10) return EINVAL;
        for (int j = 0; j < i; j++)
            if (maps[i].target == maps[j].target) return EINVAL;
    }
    /* Some spawn implementations defer exec errors to child exit 127.
     * Diagnose facts already known here; this is not an atomic exec check.
     * A later failure still belongs to the actual child outcome. */
    struct stat executable;
    if (stat(argv[0], &executable) != 0) return errno;
    if (!S_ISREG(executable.st_mode) || !(executable.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        return EACCES;
    int staged[TNY_PROCESS_MAX_FD_MAPPINGS];
    int e = stage_sources(maps, n_maps, staged);
    if (e) return e;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    e = posix_spawn_file_actions_init(&actions);
    if (e) {
        for (int i = 0; i < n_maps; i++)
            if (staged[i] >= 0) close(staged[i]);
        return e;
    }
    bool attr_ready = false;
    for (int i = 0; i < n_maps && !e; i++)
        e = posix_spawn_file_actions_adddup2(&actions, staged[i], maps[i].target);
    for (int std = STDIN_FILENO; std <= STDERR_FILENO && !e; std++) {
        bool mapped = false;
        for (int i = 0; i < n_maps; i++)
            if (maps[i].target == std) mapped = true;
        if (!mapped)
            e = posix_spawn_file_actions_addopen(&actions, std, "/dev/null",
                                                 std == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
    }
    if (!e) {
        e = posix_spawnattr_init(&attr);
        attr_ready = e == 0;
    }
    sigset_t defaults, empty;
    sigemptyset(&defaults);
    sigemptyset(&empty);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGPIPE);
    if (!e) e = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (!e) e = posix_spawnattr_setsigmask(&attr, &empty);
    if (!e) e = posix_spawnattr_setpgroup(&attr, 0);
    if (!e)
        e = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                                                POSIX_SPAWN_SETSIGMASK);
    if (!e) e = posix_spawn(pid, argv[0], &actions, &attr, argv, envp);
    if (attr_ready) posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    for (int i = 0; i < n_maps; i++)
        if (staged[i] >= 0) close(staged[i]);
    return e;
#endif
}

/* -1 = not looked up yet, 0 = no watch, else the expected supervisor pid. */
#ifndef __EMSCRIPTEN__
static pid_t g_expected_parent = -1;
#endif

void tny_process_expect_parent(pid_t parent) {
#ifdef __EMSCRIPTEN__
    (void)parent;
#else
    g_expected_parent = parent > 0 ? parent : 0;
#endif
}

bool tny_process_parent_lost(void) {
#ifdef __EMSCRIPTEN__
    return false;
#else
    if (g_expected_parent < 0) {
        const char *value = getenv(TNY_JOB_PARENT_ENV);
        char *end = NULL;
        long parsed = value && *value ? strtol(value, &end, 10) : 0;
        g_expected_parent = value && end && !*end && parsed > 1 ? (pid_t)parsed : 0;
    }
    if (g_expected_parent == 0) return false; /* ordinary CLI: no watch */
    return getppid() != g_expected_parent;
#endif
}
