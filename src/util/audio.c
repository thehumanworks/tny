/* audio.c — host OS playback seam. Anonymous, seekable input survives no
 * pathname: unlink immediately, then spawn (never fork after macOS TLS). */
#include "util/audio.h"
#include "util/tny_poll.h"
#include "util/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__) || defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
bool audio_player_available(void) { return false; }
int audio_play(const void *data, size_t len, bool (*cancelled)(void *), void *ud, char *err,
               size_t errlen) {
    (void)data;
    (void)len;
    (void)cancelled;
    (void)ud;
    snprintf(err, errlen, "audio playback is unavailable on this platform; use --output-file");
    return 1;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static const char *const players[][9] = {
#ifdef __APPLE__
    {"afplay", "/dev/fd/0", NULL},
#endif
    {"ffplay", "-nodisp", "-autoexit", "-loglevel", "error", "-i", "pipe:0", NULL},
    {"mpv", "--no-video", "--really-quiet", "-", NULL},
    {"mpg123", "-q", "-", NULL},
};

static char *player_path(size_t *index) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    for (size_t i = 0; i < sizeof players / sizeof players[0]; i++) {
        const char *p = path;
        do {
            const char *end = strchr(p, ':');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            char *dir = n ? xstrndup(p, n) : xstrdup(".");
            char *full = dir ? path_join(dir, players[i][0]) : NULL;
            free(dir);
            struct stat st;
            if (full && access(full, X_OK) == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                *index = i;
                return full;
            }
            free(full);
            p = end ? end + 1 : NULL;
        } while (p);
    }
    return NULL;
}

bool audio_player_available(void) {
    size_t i = 0;
    char *path = player_path(&i);
    bool ok = path != NULL;
    free(path);
    return ok;
}

int audio_play(const void *data, size_t len, bool (*cancelled)(void *), void *ud, char *err,
               size_t errlen) {
    size_t index = 0;
    char *player = player_path(&index);
    if (!player) {
        snprintf(err, errlen, "no audio player; install ffplay, mpv or mpg123 (afplay on macOS)");
        return 1;
    }
    const char *tmp = getenv("TMPDIR");
    char *path = path_join(tmp && *tmp ? tmp : "/tmp", "tny-speak-XXXXXX");
    int fd = path ? mkstemp(path) : -1;
    int rc = 1;
    bool linked = fd >= 0;
    if (fd < 0) goto done;
    if (unlink(path) != 0) goto done;
    linked = false;
    if (fd < 3) {
        int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        fd = copy;
        if (fd < 0) goto done;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) goto done;
    size_t offset = 0;
    while (offset < len) {
        if (cancelled && cancelled(ud)) {
            rc = 130;
            goto done;
        }
        ssize_t n = write(fd, (const char *)data + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        offset += (size_t)n;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) goto done;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) goto done;
    int e = posix_spawn_file_actions_adddup2(&actions, fd, STDIN_FILENO);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *argv[9] = {0};
    for (size_t i = 0; players[index][i]; i++) argv[i] = (char *)players[index][i];
    pid_t pid = -1;
    if (!e) e = posix_spawn(&pid, player, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (e) goto done;
    int64_t deadline = monotonic_ms() + 300000;
    for (;;) {
        int status = 0;
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            rc = cancelled && cancelled(ud)                      ? 130
                 : WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0
                                                                 : 1;
            break;
        }
        if (got < 0 && errno != EINTR) break;
        if ((cancelled && cancelled(ud)) || monotonic_ms() >= deadline) {
            rc = monotonic_ms() >= deadline ? 1 : 130;
            kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
            break;
        }
        tny_poll(NULL, 0, 25);
    }
done:
    if (fd >= 0) close(fd);
    /* Only the mkstemp owner can have created this name. */
    if (linked) unlink(path);
    free(path);
    free(player);
    if (rc) snprintf(err, errlen, rc == 130 ? "speech interrupted" : "audio playback failed");
    return rc;
}
#endif
