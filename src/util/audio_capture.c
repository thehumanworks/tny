/* External recording stays in the host OS seam, like audio playback.
 * posix_spawn is safe after macOS TLS; pipe EOF also ends capture on caller death. */
#include "util/audio_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__) || defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
bool audio_capture_available(void) { return false; }
audio_capture *audio_capture_start(const char *device, char *err, size_t errlen) {
    (void)device;
    snprintf(err, errlen, "microphone capture is unavailable on this platform; use --input-file");
    return NULL;
}
int audio_capture_fd(const audio_capture *c) {
    (void)c;
    return -1;
}
int audio_capture_read(audio_capture *c, buf_t *b, size_t limit, char *err, size_t errlen) {
    (void)c;
    (void)b;
    (void)limit;
    (void)err;
    (void)errlen;
    return -1;
}
void audio_capture_stop(audio_capture *c) { (void)c; }
void audio_capture_free(audio_capture *c) { (void)c; }
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

struct audio_capture {
    pid_t pid;
    int fd;
    bool stopping, eof;
    int64_t stop_ms;
};

static char *recorder_path(bool *alsa) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    static const char *const names[] = {
#ifndef __APPLE__
        "arecord",
#endif
        "ffmpeg"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *p = path;
        do {
            const char *end = strchr(p, ':');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            char *dir = n ? xstrndup(p, n) : xstrdup(".");
            char *full = dir ? path_join(dir, names[i]) : NULL;
            free(dir);
            struct stat st;
            if (full && access(full, X_OK) == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                *alsa = strcmp(names[i], "arecord") == 0;
                return full;
            }
            free(full);
            p = end ? end + 1 : NULL;
        } while (p);
    }
    return NULL;
}

bool audio_capture_available(void) {
    bool alsa = false;
    char *p = recorder_path(&alsa);
    bool ok = p != NULL;
    free(p);
    return ok;
}

audio_capture *audio_capture_start(const char *device, char *err, size_t errlen) {
    bool alsa = false;
    char *path = recorder_path(&alsa);
    if (!path) {
        snprintf(err, errlen, "no microphone recorder; install ffmpeg (or arecord on Linux)");
        return NULL;
    }
    if (!device || !*device) device = getenv("TNY_AUDIO_DEVICE");
    if (!device || !*device) device = "default";
    buf_t input;
    buf_init(&input);
#ifdef __APPLE__
    buf_appends(&input, ":"); /* AVFoundation: audio device only, never camera */
    const char *format = "avfoundation";
#else
    const char *format = "pulse";
#endif
    buf_appends(&input, device);
    const char *const ffmpeg[] = {
        path, "-nostdin", "-hide_banner", "-loglevel", "error",  "-f",  (char *)format,
        "-i", input.data, "-vn",          "-ac",       "1",      "-ar", "24000",
        "-t", "300",      "-f",           "s16le",     "pipe:1", NULL};
    const char *const arecord[] = {path,  "-q", "-D",     (char *)device, "-t",
                                   "raw", "-f", "S16_LE", "-r",           "24000",
                                   "-c",  "1",  "-d",     "300",          NULL};
    char *argv[24] = {0};
    const char *const *chosen = alsa ? arecord : ffmpeg;
    for (size_t i = 0; chosen[i]; i++) argv[i] = (char *)chosen[i];
    audio_capture *c = calloc(1, sizeof *c);
    int pipes[2] = {-1, -1};
    bool ok = false;
    if (!c || input.oom || pipe(pipes)) goto done;
    c->fd = -1;
    /* Keep source descriptors clear of stdin/stdout/stderr even for closed-stdio callers. */
    for (int i = 0; i < 2; i++) {
        if (pipes[i] < 3) {
            int fd = fcntl(pipes[i], F_DUPFD_CLOEXEC, 3);
            close(pipes[i]);
            pipes[i] = fd;
        }
        if (pipes[i] < 0 || fcntl(pipes[i], F_SETFD, FD_CLOEXEC) < 0) goto done;
    }
    if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) goto done;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) goto done;
    int e = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    bool have_attr = posix_spawnattr_init(&attr) == 0;
    if (!have_attr) e = EINVAL;
    if (!e) e = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (!e) e = posix_spawnattr_setpgroup(&attr, 0);
    if (!e) e = posix_spawn(&c->pid, path, &actions, &attr, argv, environ);
    if (have_attr) posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (e) goto done;
    c->fd = pipes[0];
    pipes[0] = -1;
    ok = true;
done:
    if (pipes[0] >= 0) close(pipes[0]);
    if (pipes[1] >= 0) close(pipes[1]);
    free(path);
    buf_free(&input);
    if (!ok) {
        free(c);
        snprintf(err, errlen, "cannot start microphone recorder");
        return NULL;
    }
    return c;
}

int audio_capture_fd(const audio_capture *c) { return c && !c->eof ? c->fd : -1; }

void audio_capture_stop(audio_capture *c) {
    if (!c || c->stopping) return;
    c->stopping = true;
    c->stop_ms = monotonic_ms();
    if (c->pid > 0) kill(-c->pid, SIGTERM);
}

int audio_capture_read(audio_capture *c, buf_t *b, size_t limit, char *err, size_t errlen) {
    /* A bounded batch keeps a fast/misbehaving recorder from starving the UI. */
    for (int i = 0; i < 16 && !c->eof; i++) {
        char data[8192];
        ssize_t n = read(c->fd, data, sizeof data);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        if (n < 0) goto failed;
        if (!n) {
            c->eof = true;
            break;
        }
        if (b->len > limit || (size_t)n > limit - b->len) {
            snprintf(err, errlen, "microphone audio exceeds recording limit");
            return -1;
        }
        buf_append(b, data, (size_t)n);
        if (b->oom) goto failed;
    }
    if (c->pid > 0) {
        if (c->stopping && monotonic_ms() - c->stop_ms >= 1000) kill(-c->pid, SIGKILL);
        int status = 0;
        pid_t p = waitpid(c->pid, &status, WNOHANG);
        if (p < 0 && errno != EINTR) goto failed;
        if (p > 0) {
            c->pid = 0;
            if (!c->stopping) goto failed;
        }
    }
    return c->eof && !c->pid ? 1 : 0;
failed:
    snprintf(err, errlen,
             "microphone recording failed; check the input device and microphone permission");
    return -1;
}

void audio_capture_free(audio_capture *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    if (c->pid > 0) {
        kill(-c->pid, SIGKILL);
        while (waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    free(c);
}
#endif
