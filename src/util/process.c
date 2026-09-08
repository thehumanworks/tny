#include "util/process.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
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
