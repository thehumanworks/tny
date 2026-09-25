/* acp_proc.c — ACP client transport: spawn the agent, frame JSONL both ways,
 * drain its stderr, and run one blocking request during setup.
 * Nothing here knows about tny events; see acp_events.c for that. */
#include "backends/acp/acp_client.h"
#include "util/util.h"
#include "net/net.h"
#include "util/alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/tny_poll.h"

#define ACP_RPC_TIMEOUT_MS 60000
static int ac_tx(ac_impl *o, buf_t *b) {
    if (buf_oom(b) || tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        buf_free(b);
        return -1;
    }
    int rc = acp_write_line(o->in_fd, b->data, b->len);
    buf_free(b);
    return rc;
}

int ac_tx_request(ac_impl *o, int64_t id, const char *method, const char *params) {
    if (tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        return -1;
    }
    buf_t b;
    buf_init(&b);
    acp_fmt_request(&b, id, method, params);
    return ac_tx(o, &b);
}

int ac_tx_notify(ac_impl *o, const char *method, const char *params) {
    if (tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        return -1;
    }
    buf_t b;
    buf_init(&b);
    acp_fmt_notify(&b, method, params);
    return ac_tx(o, &b);
}

int ac_tx_result(ac_impl *o, const char *id_raw, const char *result_json) {
    if (tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        return -1;
    }
    buf_t b;
    buf_init(&b);
    acp_fmt_result(&b, id_raw, result_json);
    return ac_tx(o, &b);
}

int ac_tx_error(ac_impl *o, const char *id_raw, int code, const char *msg) {
    if (tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        return -1;
    }
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":",
                id_raw ? id_raw : "null", code);
    jescape(&b, msg ? msg : "error");
    buf_appends(&b, "}}");
    return ac_tx(o, &b);
}

/* One append path for every branch, so the bounds logic exists (and is
 * unit-tested) exactly once. */
static void push_fd(struct pollfd *fds, int *n, int max, int fd, short events) {
    if (fd < 0 || *n >= max) return;
    fds[*n].fd = fd;
    fds[*n].events = events;
    fds[*n].revents = 0;
    (*n)++;
}

int ac_transport_pollfds(ac_impl *o, struct pollfd *fds, int max) {
    int n = 0;
    push_fd(fds, &n, max, o->out_fd, POLLIN);
    push_fd(fds, &n, max, o->err_fd, POLLIN);
    if (o->bridge && n < max) n += tny_acp_bridge_pollfds(o->bridge, fds + n, max - n);
    return n;
}

/* ---------- process ---------- */

bool ac_platform_supported(void) { return true; }

bool ac_on_path(const char *bin) {
    if (!bin || !*bin) return false;
    if (strchr(bin, '/')) return access(bin, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dup = xstrdup(path);
    if (!dup) return false;
    bool found = false;
    char *save = NULL;
    for (char *p = strtok_r(dup, ":", &save); p && !found; p = strtok_r(NULL, ":", &save)) {
        char *full = path_join(p, bin);
        if (!full) {
            free(dup);
            return false;
        }
        if (access(full, X_OK) == 0) found = true;
        free(full);
    }
    free(dup);
    return found;
}

int ac_spawn_agent(ac_impl *o, char *errbuf, size_t errlen) {
    char **argv = o->ctx->agent_argv;
    if (!argv || !argv[0]) {
        snprintf(errbuf, errlen,
                 "no ACP agent configured: tny --provider acp --agent CMD -- args…");
        return -1;
    }
    if (argv[0][0] != '/' && strchr(argv[0], '/')) {
        snprintf(errbuf, errlen,
                 "acp: guarded clients initialize in private scratch; configure an absolute "
                 "agent executable path or a command name on PATH instead of a relative path");
        return -1;
    }
    int inp[2], outp[2], errp[2];
    if (pipe(inp) != 0) {
        snprintf(errbuf, errlen, "pipe failed");
        return -1;
    }
    if (pipe(outp) != 0) {
        close(inp[0]);
        close(inp[1]);
        snprintf(errbuf, errlen, "pipe failed");
        return -1;
    }
    if (pipe(errp) != 0) {
        close(inp[0]);
        close(inp[1]);
        close(outp[0]);
        close(outp[1]);
        snprintf(errbuf, errlen, "pipe failed");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]);
        close(inp[1]);
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        snprintf(errbuf, errlen, "fork failed");
        return -1;
    }
    if (pid == 0) {
        unsetenv("TNY_ACP_CLEANUP_FILE"); /* never lend the parent's proof channel */
        setpgid(0, 0);                    /* own group so wrapper-forked descendants die with it */
        int stdin_fd = dup2(inp[0], STDIN_FILENO);
        if (stdin_fd < 0) _exit(127);
        int stdout_fd = dup2(outp[1], STDOUT_FILENO);
        if (stdout_fd < 0) _exit(127);
        int stderr_fd = dup2(errp[1], STDERR_FILENO);
        if (stderr_fd < 0) _exit(127);
        /* A pipe may have reused closed stdio; retain the redirected endpoints. */
        if (inp[0] > STDERR_FILENO) close(inp[0]);
        if (inp[1] > STDERR_FILENO) close(inp[1]);
        if (outp[0] > STDERR_FILENO) close(outp[0]);
        if (outp[1] > STDERR_FILENO) close(outp[1]);
        if (errp[0] > STDERR_FILENO) close(errp[0]);
        if (errp[1] > STDERR_FILENO) close(errp[1]);
        if (chdir(ac_agent_cwd(o)) != 0) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid); /* both sides set it: closes the fork/exec race */
    close(inp[0]);
    close(outp[1]);
    close(errp[1]);
    o->pid = pid;
    o->pgid = pid;
    o->in_fd = inp[1];
    o->out_fd = outp[0];
    o->err_fd = errp[0];
    if (set_nonblock(o->in_fd, true) != 0 || set_nonblock(o->out_fd, true) != 0 ||
        set_nonblock(o->err_fd, true) != 0 || fcntl(o->in_fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(o->out_fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(o->err_fd, F_SETFD, FD_CLOEXEC) < 0) {
        snprintf(errbuf, errlen, "acp: could not configure agent transport descriptors");
        if (ac_guarded(o)) return -1; /* caller retains the whole tree for proven cleanup */
        if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        close(o->in_fd);
        close(o->out_fd);
        close(o->err_fd);
        o->in_fd = o->out_fd = o->err_fd = -1;
        o->pid = o->pgid = 0;
        return -1;
    }
    return 0;
}

/* Drain diagnostics without echoing untrusted adapter logs: they can contain
 * credentials. Bounded work per dispatch prevents a noisy child starving MCP. */
static void drain_stderr(ac_impl *o) {
    if (o->err_fd < 0) return;
    char tmp[4096];
    for (int i = 0; i < 16; i++) {
        ssize_t n = read(o->err_fd, tmp, sizeof tmp);
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) {
            close(o->err_fd);
            o->err_fd = -1;
        }
        break;
    }
}

/* Returns true when the doc was stored (ownership transferred). */
static bool handle_message(ac_impl *o, yyjson_doc *doc) {
    yyjson_val *msg = yyjson_doc_get_root(doc);
    if (!msg || !yyjson_is_obj(msg)) return false;
    const char *method = jget_str(msg, "method");
    if (method) {
        yyjson_val *params = jget(msg, "params");
        if (strcmp(method, "session/update") == 0) {
            ac_handle_update(o, params);
            return false;
        }
        if (jget(msg, "id")) ac_handle_agent_request(o, msg, method, params);
        return false;
    }
    /* response */
    int64_t id = acp_id_num(msg);
    if (o->wait_id >= 0 && id == o->wait_id) {
        o->wait_doc = doc;
        o->wait_id = -1;
        return true;
    }
    if (o->turn_active && id == o->prompt_id) {
        o->prompt_id = -1;
        ac_handle_prompt_response(o, msg);
    }
    return false;
}

/* Process each bounded read immediately so complete small frames cannot
 * accumulate without bound behind a perpetually readable producer. */
int ac_pump_reads(ac_impl *o) {
    drain_stderr(o);
    if (o->bridge && tny_acp_bridge_dispatch(o->bridge) != 0) return -1;
    if (o->out_fd < 0) return -1;
    bool eof = false;
    char tmp[16384];
    for (int reads = 0; reads < 16; reads++) {
        ssize_t n = read(o->out_fd, tmp, sizeof tmp);
        if (n > 0) acp_reader_feed(&o->out_r, tmp, (size_t)n);
        else if (n == 0) eof = true;
        else if (errno == EINTR) continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
        if (o->out_r.overflow) return -2;
        if (tny_alloc_scope_failed()) goto oom;
        for (;;) {
            size_t len = 0;
            char *line = acp_reader_next(&o->out_r, &len);
            if (!line) break;
            if (!len) {
                free(line);
                continue;
            }
            yyjson_doc *doc = jparse(line, len);
            free(line);
            if (!doc) {
                if (tny_alloc_scope_failed()) goto oom;
                return -4;
            }
            yyjson_val *root = yyjson_doc_get_root(doc);
            const char *version = jget_str(root, "jsonrpc");
            yyjson_val *id = jget(root, "id");
            bool valid_id = !id || yyjson_is_str(id) || yyjson_is_int(id);
            bool valid =
                yyjson_is_obj(root) && version && strcmp(version, "2.0") == 0 && valid_id &&
                (jget_str(root, "method") || (id && (jget(root, "result") || jget(root, "error"))));
            if (!valid) {
                yyjson_doc_free(doc);
                return -4;
            }
            if (!handle_message(o, doc)) yyjson_doc_free(doc);
            if (tny_alloc_scope_failed()) goto oom;
            if (o->out_fd < 0 && !o->turn_active)
                return 0; /* managed completion closed transport */
        }
        if (n <= 0) break;
    }
    if (tny_alloc_scope_failed()) goto oom;
    if (eof && o->out_r.buf.len) return -4;
    return eof ? -1 : 0;
oom:
    tny_alloc_provider_failed();
    return -3;
}

/* Exit status of the agent once its stdout is closed, or -1 if it is still
 * running / already reaped. Bounded wait so a wedged child cannot hang tny. */
int ac_reap_agent(ac_impl *o) {
    if (ac_guarded(o)) return -1; /* cleanup must retain the owned root identity */
    for (int i = 0; i < 20 && o->pid > 0; i++) {
        int status = 0;
        pid_t r = waitpid(o->pid, &status, WNOHANG);
        if (r == o->pid) {
            o->pid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0) {
            o->pid = 0;
            return -1;
        }
        tny_poll(NULL, 0, 10);
    }
    return -1;
}

/* ---------- blocking request (setup only, never during a turn) ---------- */

yyjson_doc *ac_rpc(ac_impl *o, const char *method, const char *params, char *errbuf,
                   size_t errlen) {
    int64_t id = o->next_id++;
    if (ac_tx_request(o, id, method, params) != 0) {
        snprintf(errbuf, errlen, "acp: agent closed its input during %s", method);
        return NULL;
    }
    o->wait_id = id;
    o->wait_doc = NULL;
    int timeout = ACP_RPC_TIMEOUT_MS;
    const char *override = getenv("TNY_ACP_RPC_TIMEOUT_MS");
    if (override) {
        char *end = NULL;
        long value = strtol(override, &end, 10);
        if (end && !*end && value >= 50 && value <= ACP_RPC_TIMEOUT_MS) timeout = (int)value;
    }
    int64_t deadline = monotonic_ms() + timeout;
    while (!o->wait_doc) {
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) {
            o->wait_id = -1;
            snprintf(errbuf, errlen, "acp: agent did not answer %s in time", method);
            return NULL;
        }
        struct pollfd fds[TNY_BACKEND_POLLFD_MAX];
        int n = ac_transport_pollfds(o, fds, TNY_BACKEND_POLLFD_MAX);
        int pr = tny_poll(fds, (nfds_t)n, (int)(left > 200 ? 200 : left));
        if (pr < 0 && errno == EINTR) continue;
        int rc = ac_pump_reads(o);
        if (rc == -3) {
            o->wait_id = -1;
            snprintf(errbuf, errlen, "acp: out of memory");
            return NULL;
        }
        if (rc == -4) {
            o->wait_id = -1;
            snprintf(errbuf, errlen, "acp: malformed JSON-RPC message during %s", method);
            return NULL;
        }
        if (rc == -2) {
            o->wait_id = -1;
            snprintf(errbuf, errlen, "acp: agent sent a message over the 8 MiB cap");
            return NULL;
        }
        if (rc == -1 && !o->wait_doc) {
            o->wait_id = -1;
            int code = ac_reap_agent(o);
            if (code == 127)
                snprintf(errbuf, errlen,
                         "acp: cannot execute agent '%.80s' "
                         "(not found or not executable)",
                         o->ctx->agent_argv ? o->ctx->agent_argv[0] : "?");
            else if (code >= 0)
                snprintf(errbuf, errlen, "acp: agent exited (status %d) during %s", code, method);
            else snprintf(errbuf, errlen, "acp: agent closed the connection during %s", method);
            return NULL;
        }
    }
    yyjson_doc *doc = o->wait_doc;
    o->wait_doc = NULL;
    yyjson_val *err = jget(yyjson_doc_get_root(doc), "error");
    if (err) {
        snprintf(errbuf, errlen,
                 "acp: %s failed (code %lld; authenticate the agent separately if required)",
                 method, (long long)jget_int(err, "code", ACP_E_INTERNAL));
        yyjson_doc_free(doc);
        return NULL;
    }
    return doc;
}
