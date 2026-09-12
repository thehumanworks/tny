#include "util/process.h"

#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <spawn.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <dirent.h>
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
#ifdef __EMSCRIPTEN__
    (void)argv;
    (void)envp;
    (void)in_fd;
    (void)out_fd;
    (void)pid;
    return ENOTSUP;
#else
    if (!argv || !argv[0] || argv[0][0] != '/' || !envp || in_fd < 0 || out_fd < 0 || !pid)
        return EINVAL;
    /* Some spawn implementations defer exec errors to child exit 127.
     * Diagnose facts already known here; this is not an atomic exec check.
     * A later failure still belongs to the actual child outcome. */
    struct stat executable;
    if (stat(argv[0], &executable) != 0) return errno;
    if (!S_ISREG(executable.st_mode) || !(executable.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        return EACCES;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int e = posix_spawn_file_actions_init(&actions);
    if (e) return e;
    bool attr_ready = false;
    e = posix_spawn_file_actions_adddup2(&actions, in_fd, STDIN_FILENO);
    if (!e) e = posix_spawn_file_actions_adddup2(&actions, out_fd, STDOUT_FILENO);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
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
    return e;
#endif
}
