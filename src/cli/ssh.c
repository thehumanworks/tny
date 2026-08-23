/* ssh.c — process-level SSH execution boundary.
 *
 * SSH mode deliberately delegates the complete tny invocation to the trusted
 * remote host before local config/session/backend initialization. This makes
 * remote execution an application invariant instead of something individual
 * tools or agents must remember to honor.
 */
#include "cli/cli.h"
#include "util/util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *host;
    char port[6];
} ssh_target;

static void ssh_target_free(ssh_target *t) {
    free(t->host);
    t->host = NULL;
}

static bool bad_target_char(unsigned char c) {
    return c <= 0x20 || c == 0x7f;
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

/* Accept user@host, host:port, user@host:port and [ipv6]:port. The returned
 * host is suitable for one argv element to OpenSSH; no local shell is used. */
static int ssh_target_parse(const char *spec, ssh_target *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!spec || !*spec) {
        snprintf(err, errlen, "empty SSH target");
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)spec; *p; p++) {
        if (bad_target_char(*p)) {
            snprintf(err, errlen, "SSH target must not contain whitespace or control characters");
            return -1;
        }
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
            /* Bare IPv6 is ambiguous with :port; require brackets. */
            if (strchr(hostpart, ':') != colon) {
                snprintf(err, errlen, "IPv6 SSH targets with a port must use [addr]:port");
                return -1;
            }
            port = colon + 1;
            hostlen = (size_t)(colon - spec);
        }
    }
    if (hostlen == 0) {
        snprintf(err, errlen, "invalid SSH target '%s'", spec);
        return -1;
    }
    if (port && parse_port(port, out->port) < 0) {
        snprintf(err, errlen, "invalid SSH port in '%s' (expected 1..65535)", spec);
        return -1;
    }
    out->host = xstrndup(spec, hostlen);
    return out->host ? 0 : -1;
}

/* POSIX-shell quote one remote argv element. OpenSSH transmits a remote
 * command string to the server-side shell; quoting here prevents arguments
 * such as prompts, paths and model names from being reinterpreted remotely. */
static void append_remote_arg(buf_t *b, const char *s) {
    buf_appends(b, "'");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buf_appends(b, "'\\''");
        else buf_append(b, p, 1);
    }
    buf_appends(b, "'");
}

static int ssh_exec(const char *spec, int argc, char **argv, int skip_i) {
    ssh_target t;
    char err[256];
    if (ssh_target_parse(spec, &t, err, sizeof err) < 0) {
        fprintf(stderr, "tny: --ssh: %s\n", err);
        return 1;
    }

    buf_t cmd;
    buf_init(&cmd);
    append_remote_arg(&cmd, "tny");
    for (int i = 1; i < argc; i++) {
        if (i == skip_i) { i++; continue; } /* remove --ssh TARGET */
        buf_appends(&cmd, " ");
        append_remote_arg(&cmd, argv[i]);
    }

    if (t.port[0]) {
        execlp("ssh", "ssh", "-p", t.port, "--", t.host, cmd.data, (char *)NULL);
    } else {
        execlp("ssh", "ssh", "--", t.host, cmd.data, (char *)NULL);
    }
    int e = errno;
    fprintf(stderr, "tny: could not execute ssh for %s: %s\n", spec, strerror(e));
    buf_free(&cmd);
    ssh_target_free(&t);
    return 1;
}

/* Return -1 when no leading --ssh is present. Otherwise this function only
 * returns on validation/exec failure; successful delegation replaces tny. */
int cli_ssh_maybe_delegate(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0 || a[0] != '-') break;
        if (strcmp(a, "--ssh") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tny: --ssh requires user@host[:port]\n");
                return 1;
            }
            return ssh_exec(argv[i + 1], argc, argv, i);
        }
        /* Skip values for the existing leading globals so a value beginning
         * with '-' is not mistaken for another global token. */
        if (strcmp(a, "--provider") == 0 || strcmp(a, "--backend") == 0 ||
            strcmp(a, "--cwd") == 0 || strcmp(a, "--model") == 0 ||
            strcmp(a, "--effort") == 0 || strcmp(a, "--reasoning-effort") == 0 ||
            strcmp(a, "--add-dir") == 0 || strcmp(a, "--permission-mode") == 0 ||
            strcmp(a, "--resume") == 0 || strcmp(a, "--bridge-bin") == 0 ||
            strcmp(a, "--codex-ws") == 0 || strcmp(a, "--codex-bin") == 0 ||
            strcmp(a, "--ws-token-file") == 0 || strcmp(a, "--base-url") == 0 ||
            strcmp(a, "--api-key-env") == 0 || strcmp(a, "--wire-api") == 0) {
            if (i + 1 < argc) i++;
        }
    }
    return -1;
}

int cli_ssh_exec_tui(const char *spec) {
    char *av[] = {(char *)"tny", (char *)"--ssh", (char *)spec, NULL};
    return ssh_exec(spec, 3, av, 1);
}
