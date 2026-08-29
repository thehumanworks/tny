/* ssh.c — remote tool runtime over one OpenSSH ControlMaster (docs/adr/0022).
 *
 * Nothing here is a tool: this file owns the connection and a single
 * "run this sh script remotely" primitive. tools_ssh.c maps each workspace
 * tool onto it. Authentication and host-key policy stay with the user's
 * OpenSSH config; tny never weakens them. */
#include "core/ssh.h"
#include "core/config.h"
#include "core/instructions.h"
#include "util/tny_poll.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SSH_MAX_ARGS 24

void ssh_shell_quote(buf_t *b, const char *s) {
    buf_appends(b, "'");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buf_appends(b, "'\\''");
        else buf_append(b, p, 1);
    }
    buf_appends(b, "'");
}

static int parse_port(const char *s, char out[6]) {
    if (!s || !*s) return -1;
    unsigned long n = 0;
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++, len++) {
        if (!isdigit(*p) || len >= 5) return -1;
        n = n * 10u + (unsigned long)(*p - '0');
        if (n > 65535u) return -1;
    }
    if (n == 0) return -1;
    snprintf(out, 6, "%lu", n);
    return 0;
}

/* Accept user@host, host:port, user@host:port and [ipv6]:port. The host
 * becomes one argv element to OpenSSH; no local shell is involved. */
int ssh_target_set(tny_ctx *ctx, const char *spec, char *err, size_t errlen) {
    if (!spec || !*spec) {
        snprintf(err, errlen, "empty SSH target");
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)spec; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) {
            snprintf(err, errlen, "SSH target must not contain whitespace or control characters");
            return -1;
        }
    }
    if (spec[0] == '-') {
        snprintf(err, errlen, "invalid SSH target '%s'", spec);
        return -1;
    }
    const char *at = strrchr(spec, '@');
    const char *hostpart = at ? at + 1 : spec;
    if (at == spec || !*hostpart) {
        snprintf(err, errlen, "invalid SSH target '%s'", spec);
        return -1;
    }
    const char *port = NULL;
    size_t hostlen = strlen(spec);
    if (*hostpart == '[') {
        const char *close = strchr(hostpart, ']');
        if (!close || close == hostpart + 1) {
            snprintf(err, errlen, "invalid bracketed SSH host '%s'", spec);
            return -1;
        }
        if (close[1] == ':') port = close + 2;
        else if (close[1] != '\0') {
            snprintf(err, errlen, "invalid SSH target suffix in '%s'", spec);
            return -1;
        }
        if (port) hostlen = (size_t)(close - spec + 1);
    } else {
        const char *colon = strrchr(hostpart, ':');
        if (colon) {
            if (strchr(hostpart, ':') != colon) {
                snprintf(err, errlen, "IPv6 SSH targets with a port must use [addr]:port");
                return -1;
            }
            port = colon + 1;
            hostlen = (size_t)(colon - spec);
        }
    }
    char portbuf[6] = {0};
    if (port && parse_port(port, portbuf) < 0) {
        snprintf(err, errlen, "invalid SSH port in '%s' (expected 1..65535)", spec);
        return -1;
    }
    free(ctx->ssh_host);
    ctx->ssh_host = xstrndup(spec, hostlen);
    memcpy(ctx->ssh_port, portbuf, sizeof portbuf);
    return 0;
}

/* ~/.tny/ssh/%C — OpenSSH hashes host/port/user into %C, so one socket per
 * target and nothing user-controlled lands in the path. */
static char *control_path(void) {
    char *home = path_home();
    char *dir = path_join(home, ".tny/ssh");
    mkdir_p(dir);
    chmod(dir, 0700);
    char *cp = path_join(dir, "%C");
    free(home);
    free(dir);
    return cp;
}

static int base_argv(tny_ctx *ctx, const char **av, int n, bool batch) {
    av[n++] = "ssh";
    av[n++] = "-o";
    av[n++] = "ControlMaster=auto";
    av[n++] = "-o";
    av[n++] = "ControlPersist=600";
    av[n++] = "-o";
    av[n++] = ctx->ssh_control; /* "ControlPath=..." */
    if (batch) {
        av[n++] = "-o";
        av[n++] = "BatchMode=yes";
    }
    if (ctx->ssh_port[0]) {
        av[n++] = "-p";
        av[n++] = ctx->ssh_port;
    }
    av[n++] = "--";
    av[n++] = ctx->ssh_host;
    return n;
}

static void ensure_control(tny_ctx *ctx) {
    if (ctx->ssh_control) return;
    char *cp = control_path();
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "ControlPath=%s", cp);
    free(cp);
    ctx->ssh_control = buf_detach(&b);
}

int ssh_run(tny_ctx *ctx, const char *script, const char *in, size_t inlen, int timeout_s,
            size_t out_cap, buf_t *out, bool *truncated, bool *timed_out) {
    *truncated = false;
    *timed_out = false;
    if (!ctx->ssh_host) {
        buf_appends(out, "ssh: not connected");
        return 255;
    }
    ensure_control(ctx);
    /* The remote login shell (bash, zsh, fish, …) receives one string; wrap
     * the script so POSIX sh interprets it regardless of that shell. */
    buf_t cmd;
    buf_init(&cmd);
    buf_appends(&cmd, "cd ");
    ssh_shell_quote(&cmd, ctx->ssh_cwd ? ctx->ssh_cwd : ".");
    buf_appends(&cmd, " && exec sh -c ");
    ssh_shell_quote(&cmd, script);

    const char *av[SSH_MAX_ARGS];
    int n = base_argv(ctx, av, 0, true);
    av[n++] = cmd.data;
    av[n] = NULL;

    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) {
        buf_free(&cmd);
        buf_appends(out, "ssh: pipe failed");
        return 255;
    }
    if (pipe(outpipe) != 0) {
        close(inpipe[0]);
        close(inpipe[1]);
        buf_free(&cmd);
        buf_appends(out, "ssh: pipe failed");
        return 255;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        buf_free(&cmd);
        buf_appends(out, "ssh: fork failed");
        return 255;
    }
    if (pid == 0) {
        dup2(inpipe[0], 0);
        dup2(outpipe[1], 1);
        dup2(outpipe[1], 2);
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        setpgid(0, 0);
        execvp("ssh", (char *const *)av);
        _exit(255);
    }
    buf_free(&cmd);
    close(inpipe[0]);
    close(outpipe[1]);
    /* Feed stdin without deadlocking on a chatty remote: non-blocking writes
     * interleaved with reads. */
    fcntl(inpipe[1], F_SETFL, fcntl(inpipe[1], F_GETFL) | O_NONBLOCK);
    size_t written = 0;
    if (!in) inlen = 0;
    if (inlen == 0) {
        close(inpipe[1]);
        inpipe[1] = -1;
    }

    int64_t deadline = timeout_s > 0 ? now_ms() + (int64_t)timeout_s * 1000 : 0;
    for (;;) {
        struct pollfd pf[2];
        int np = 0;
        pf[np++] = (struct pollfd){outpipe[0], POLLIN, 0};
        if (inpipe[1] >= 0) pf[np++] = (struct pollfd){inpipe[1], POLLOUT, 0};
        int wait = 500;
        if (deadline) {
            int64_t left = deadline - now_ms();
            if (left <= 0) {
                *timed_out = true;
                break;
            }
            if (left < wait) wait = (int)left;
        }
        int pr = tny_poll(pf, (nfds_t)np, wait);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (np == 2 && (pf[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(inpipe[1], in + written, inlen - written);
            if (w > 0) written += (size_t)w;
            if (w < 0 && errno != EAGAIN && errno != EINTR) {
                close(inpipe[1]);
                inpipe[1] = -1;
            }
            if (written >= inlen) {
                close(inpipe[1]);
                inpipe[1] = -1;
            }
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char tmp[8192];
            ssize_t r = read(outpipe[0], tmp, sizeof tmp);
            if (r == 0) break;
            if (r < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            if (out->len < out_cap) buf_append(out, tmp, (size_t)r);
            else *truncated = true;
        }
    }
    if (inpipe[1] >= 0) close(inpipe[1]);
    close(outpipe[0]);
    if (*timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (*timed_out) return 124;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int ssh_connect(tny_ctx *ctx, char *err, size_t errlen) {
    if (!ctx->ssh_host) {
        snprintf(err, errlen, "no SSH target");
        return -1;
    }
    ensure_control(ctx);
    /* Interactive master: inherits the tty so OpenSSH can prompt. */
    const char *av[SSH_MAX_ARGS];
    int n = 0;
    av[n++] = "ssh";
    av[n++] = "-o";
    av[n++] = "ControlMaster=auto";
    av[n++] = "-o";
    av[n++] = "ControlPersist=600";
    av[n++] = "-o";
    av[n++] = ctx->ssh_control;
    if (ctx->ssh_port[0]) {
        av[n++] = "-p";
        av[n++] = ctx->ssh_port;
    }
    av[n++] = "--";
    av[n++] = ctx->ssh_host;
    av[n++] = "true";
    av[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "fork failed");
        return -1;
    }
    if (pid == 0) {
        execvp("ssh", (char *const *)av);
        fprintf(stderr, "tny: could not execute ssh: %s\n", strerror(errno));
        _exit(255);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    if (code != 0) {
        snprintf(err, errlen, "ssh %s failed (exit %d)", ctx->ssh_host, code);
        return -1;
    }
    /* Resolve the remote working directory once so every tool sees absolute
     * paths and the permission log is unambiguous. */
    char *unresolved = ctx->ssh_cwd;
    ctx->ssh_cwd = NULL; /* ssh_run must not cd into the unresolved dir first */
    const char *want = unresolved ? unresolved : ".";
    buf_t script, out;
    buf_init(&script);
    buf_init(&out);
    buf_appends(&script, "cd ");
    /* A leading ~ means the *remote* home, never the local one; sh only
     * expands ~ unquoted, so rewrite it to "$HOME" (cf. rpath in
     * tools_ssh.c). */
    if (want[0] == '~' && (want[1] == '/' || want[1] == '\0')) {
        buf_appends(&script, "\"$HOME\"");
        if (want[1]) ssh_shell_quote(&script, want + 1);
    } else ssh_shell_quote(&script, want);
    buf_appends(&script, " && pwd");
    bool tr, to;
    int rc = ssh_run(ctx, script.data, NULL, 0, 30, 4096, &out, &tr, &to);
    buf_free(&script);
    if (rc != 0 || !out.len) {
        snprintf(err, errlen, "remote directory %s: %s", want,
                 out.len ? out.data : "not reachable");
        free(unresolved);
        buf_free(&out);
        return -1;
    }
    free(unresolved);
    while (out.len && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == '\r'))
        out.data[--out.len] = 0;
    ctx->ssh_cwd = buf_detach(&out);
    /* Project instructions follow the tool workspace (docs/adr/0040). */
    (void)instructions_refresh(ctx);
    return 0;
}

void ssh_disconnect(tny_ctx *ctx) {
    if (ctx->ssh_host && ctx->ssh_control) {
        const char *av[SSH_MAX_ARGS];
        int n = 0;
        av[n++] = "ssh";
        av[n++] = "-o";
        av[n++] = ctx->ssh_control;
        av[n++] = "-O";
        av[n++] = "exit";
        if (ctx->ssh_port[0]) {
            av[n++] = "-p";
            av[n++] = ctx->ssh_port;
        }
        av[n++] = "--";
        av[n++] = ctx->ssh_host;
        av[n] = NULL;
        pid_t pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) {
                dup2(dn, 0);
                dup2(dn, 1);
                dup2(dn, 2);
            }
            execvp("ssh", (char *const *)av);
            _exit(255);
        }
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0);
        }
    }
    free(ctx->ssh_host);
    ctx->ssh_host = NULL;
    free(ctx->ssh_cwd);
    ctx->ssh_cwd = NULL;
    free(ctx->ssh_control);
    ctx->ssh_control = NULL;
    ctx->ssh_port[0] = 0;
    (void)instructions_refresh(ctx);
}
