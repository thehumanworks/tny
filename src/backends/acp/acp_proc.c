/* acp_proc.c — ACP client transport: spawn the agent, frame JSONL both ways,
 * drain its stderr, and run one blocking request during setup.
 * Nothing here knows about tny events; see acp_events.c for that. */
#include "backends/acp/acp_client.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ACP_RPC_TIMEOUT_MS 60000

/* ---------- process ---------- */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

bool ac_on_path(const char *bin) {
    if (!bin || !*bin) return false;
    if (strchr(bin, '/')) return access(bin, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dup = xstrdup(path);
    bool found = false;
    for (char *p = strtok(dup, ":"); p && !found; p = strtok(NULL, ":")) {
        char *full = path_join(p, bin);
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
    int inp[2], outp[2], errp[2];
    if (pipe(inp) != 0) { snprintf(errbuf, errlen, "pipe failed"); return -1; }
    if (pipe(outp) != 0) {
        close(inp[0]); close(inp[1]);
        snprintf(errbuf, errlen, "pipe failed");
        return -1;
    }
    if (pipe(errp) != 0) {
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        snprintf(errbuf, errlen, "pipe failed");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        snprintf(errbuf, errlen, "fork failed");
        return -1;
    }
    if (pid == 0) {
        dup2(inp[0], 0);
        dup2(outp[1], 1);
        dup2(errp[1], 2);
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        if (chdir(o->ctx->cwd) != 0) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    close(errp[1]);
    o->pid = pid;
    o->in_fd = inp[1];
    o->out_fd = outp[0];
    o->err_fd = errp[0];
    set_nonblock(o->out_fd);
    set_nonblock(o->err_fd);
    return 0;
}

/* Forward whatever the child logged, one prefixed line at a time. */
static void drain_stderr(ac_impl *o) {
    if (o->err_fd < 0) return;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(o->err_fd, tmp, sizeof tmp);
        if (n > 0) { acp_reader_feed(&o->err_r, tmp, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    char *line;
    while ((line = acp_reader_next(&o->err_r, NULL)) != NULL) {
        if (*line) fprintf(stderr, "acp: %.500s\n", line);
        free(line);
    }
    if (o->err_r.overflow) o->err_r.overflow = false; /* logs are best effort */
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

/* Read whatever is pending. Returns -1 on EOF/error of the agent's stdout. */
int ac_pump_reads(ac_impl *o) {
    drain_stderr(o);
    if (o->out_fd < 0) return -1;
    char tmp[16384];
    bool eof = false;
    for (;;) {
        ssize_t n = read(o->out_fd, tmp, sizeof tmp);
        if (n > 0) { acp_reader_feed(&o->out_r, tmp, (size_t)n); continue; }
        if (n == 0) { eof = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        eof = true;
        break;
    }
    for (;;) {
        size_t len = 0;
        char *line = acp_reader_next(&o->out_r, &len);
        if (!line) break;
        if (!len) { free(line); continue; }
        yyjson_doc *doc = jparse(line, len);
        free(line);
        if (!doc) continue; /* a malformed line must not kill the loop */
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_arr(root)) { /* v2 batch: process each element */
            size_t idx, max;
            yyjson_val *el;
            yyjson_arr_foreach(root, idx, max, el) {
                char *one = jwrite_val(el);
                if (!one) continue;
                yyjson_doc *sub = jparse(one, strlen(one));
                free(one);
                if (sub && !handle_message(o, sub)) yyjson_doc_free(sub);
            }
            yyjson_doc_free(doc);
            continue;
        }
        if (!handle_message(o, doc)) yyjson_doc_free(doc);
    }
    if (o->out_r.overflow) return -2;
    return eof ? -1 : 0;
}

/* Exit status of the agent once its stdout is closed, or -1 if it is still
 * running / already reaped. Bounded wait so a wedged child cannot hang tny. */
int ac_reap_agent(ac_impl *o) {
    for (int i = 0; i < 20 && o->pid > 0; i++) {
        int status = 0;
        pid_t r = waitpid(o->pid, &status, WNOHANG);
        if (r == o->pid) {
            o->pid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0) { o->pid = 0; return -1; }
        poll(NULL, 0, 10);
    }
    return -1;
}

/* ---------- blocking request (setup only, never during a turn) ---------- */

yyjson_doc *ac_rpc(ac_impl *o, const char *method, const char *params,
                          char *errbuf, size_t errlen) {
    int64_t id = o->next_id++;
    if (acp_send_request(o->in_fd, id, method, params) != 0) {
        snprintf(errbuf, errlen, "acp: agent closed its input during %s", method);
        return NULL;
    }
    o->wait_id = id;
    o->wait_doc = NULL;
    int64_t deadline = now_ms() + ACP_RPC_TIMEOUT_MS;
    while (!o->wait_doc) {
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            o->wait_id = -1;
            snprintf(errbuf, errlen, "acp: agent did not answer %s in time", method);
            return NULL;
        }
        struct pollfd fds[2];
        int n = 0;
        fds[n].fd = o->out_fd; fds[n].events = POLLIN; fds[n++].revents = 0;
        if (o->err_fd >= 0) { fds[n].fd = o->err_fd; fds[n].events = POLLIN; fds[n++].revents = 0; }
        int pr = poll(fds, (nfds_t)n, (int)(left > 200 ? 200 : left));
        if (pr < 0 && errno == EINTR) continue;
        int rc = ac_pump_reads(o);
        if (rc == -2) {
            o->wait_id = -1;
            snprintf(errbuf, errlen, "acp: agent sent a message over the 8 MiB cap");
            return NULL;
        }
        if (rc == -1 && !o->wait_doc) {
            o->wait_id = -1;
            int code = ac_reap_agent(o);
            if (code == 127)
                snprintf(errbuf, errlen, "acp: cannot execute agent '%.80s' "
                         "(not found or not executable)",
                         o->ctx->agent_argv ? o->ctx->agent_argv[0] : "?");
            else if (code >= 0)
                snprintf(errbuf, errlen, "acp: agent exited (status %d) during %s",
                         code, method);
            else
                snprintf(errbuf, errlen, "acp: agent closed the connection during %s",
                         method);
            return NULL;
        }
    }
    yyjson_doc *doc = o->wait_doc;
    o->wait_doc = NULL;
    yyjson_val *err = jget(yyjson_doc_get_root(doc), "error");
    if (err) {
        const char *m = jget_str(err, "message");
        snprintf(errbuf, errlen, "acp: %s failed: %.200s", method,
                 m ? m : "agent returned an error");
        yyjson_doc_free(doc);
        return NULL;
    }
    return doc;
}

