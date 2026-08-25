/* extensions.c — deterministic discovery + one optional Python hook host.
 *
 * The child protocol is bounded JSONL.  No provider wire object crosses this
 * boundary: callers pass a normalized event object assembled by the runtime.
 * Child stderr is drained and discarded so extension logs cannot leak secrets
 * into tny's own diagnostics. */
#include "core/extensions.h"

#include "json/json.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define EXT_PROTOCOL 1
#define EXT_MAX_ENTRIES 64u
#define EXT_MAX_EVENT_NAME 80u
#define EXT_MAX_EVENT_JSON (128u * 1024u)
#define EXT_MAX_ACTION_TEXT (64u * 1024u)
#define EXT_MAX_LINE (256u * 1024u)
#define EXT_DEFAULT_TIMEOUT_MS 2000
#define EXT_MAX_TIMEOUT_MS 600000

typedef struct {
    char *name;
    char *path;
} ext_entry;

typedef struct {
    char *event;
    char *handler_id;
    char *extension;
} ext_subscription;

struct tny_extensions {
    char *tny_dir;
    char *cwd;
    int timeout_ms;
    tny_extensions_state state;
    char status[160];

    ext_entry *entries;
    size_t n_entries;
    ext_subscription *subscriptions;
    size_t n_subscriptions;
    tny_extension_failure *pending_failures;
    size_t n_pending_failures;
    bool unavailable_reported;

    pid_t pid;
    int in_fd;
    int out_fd;
    int err_fd;
    int64_t next_id;
    buf_t input;
};

typedef struct {
    char *event;
    char *handler_id;
    char *extension;
} ext_call;

static void set_status(tny_extensions *x, tny_extensions_state state,
                       const char *message) {
    x->state = state;
    snprintf(x->status, sizeof x->status, "%s", message ? message : "");
}

static char *dup_cap(const char *s, size_t cap) {
    if (!s) return NULL;
    size_t n = strlen(s);
    return xstrndup(s, n < cap ? n : cap);
}

static bool regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int cmp_entry(const void *a, const void *b) {
    const ext_entry *ea = a, *eb = b;
    int by_name = strcmp(ea->name, eb->name);
    return by_name ? by_name : strcmp(ea->path, eb->path);
}

static char *path_parent(const char *path) {
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (!slash) { free(copy); return xstrdup("."); }
    if (slash == copy) slash[1] = 0;
    else *slash = 0;
    return copy;
}

static char *entry_name(const char *dir_name, bool index_file) {
    if (index_file) return xstrdup(dir_name);
    size_t n = strlen(dir_name);
    return xstrndup(dir_name, n >= 3 ? n - 3 : n);
}

static void add_entry(tny_extensions *x, const char *name, const char *path,
                      bool index_file) {
    if (x->n_entries >= EXT_MAX_ENTRIES) return;
    char *absolute = path_abs(path);
    if (!absolute || !regular_file(absolute)) { free(absolute); return; }
    ext_entry *next = realloc(x->entries,
                              sizeof *next * (x->n_entries + 1));
    if (!next) { free(absolute); return; }
    x->entries = next;
    ext_entry *entry = &x->entries[x->n_entries++];
    entry->name = entry_name(name, index_file);
    if (index_file) {
        entry->path = path_parent(absolute); /* host loads packages as packages */
        free(absolute);
    } else {
        entry->path = absolute;
    }
}

static void pending_failure_add(tny_extensions *x, const char *extension,
                                const char *code, const char *message) {
    tny_extension_failure *next = realloc(
        x->pending_failures, sizeof *next * (x->n_pending_failures + 1));
    if (!next) return;
    x->pending_failures = next;
    tny_extension_failure *failure = &x->pending_failures[x->n_pending_failures++];
    memset(failure, 0, sizeof *failure);
    failure->extension = dup_cap(extension ? extension : "extension", 128);
    failure->handler_id = xstrdup("");
    failure->event = xstrdup("");
    failure->code = dup_cap(code ? code : "load_error", 64);
    failure->message = dup_cap(message ? message : "extension failed to load", 512);
}

static void drop_name_collisions(tny_extensions *x) {
    size_t write = 0;
    for (size_t i = 0; i < x->n_entries;) {
        size_t end = i + 1;
        while (end < x->n_entries &&
               strcmp(x->entries[i].name, x->entries[end].name) == 0)
            end++;
        if (end - i > 1) {
            pending_failure_add(x, x->entries[i].name, "name_collision",
                                "both file and directory forms exist; neither was loaded");
            for (size_t j = i; j < end; j++) {
                free(x->entries[j].name);
                free(x->entries[j].path);
            }
        } else {
            if (write != i) x->entries[write] = x->entries[i];
            write++;
        }
        i = end;
    }
    x->n_entries = write;
}

static void discover(tny_extensions *x) {
    char *root = path_join(x->tny_dir, "extensions");
    DIR *dir = opendir(root);
    if (!dir) { free(root); return; }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        char *path = path_join(root, de->d_name);
        size_t n = strlen(de->d_name);
        if (n > 3 && strcmp(de->d_name + n - 3, ".py") == 0) {
            add_entry(x, de->d_name, path, false);
        } else if (dir_exists(path)) {
            char *index = path_join(path, "index.py");
            add_entry(x, de->d_name, index, true);
            free(index);
        }
        free(path);
    }
    closedir(dir);
    free(root);
    if (x->n_entries > 1)
        qsort(x->entries, x->n_entries, sizeof *x->entries, cmp_entry);
    drop_name_collisions(x);
}

static void subscriptions_clear(tny_extensions *x) {
    for (size_t i = 0; i < x->n_subscriptions; i++) {
        free(x->subscriptions[i].event);
        free(x->subscriptions[i].handler_id);
        free(x->subscriptions[i].extension);
    }
    free(x->subscriptions);
    x->subscriptions = NULL;
    x->n_subscriptions = 0;
}

#ifndef __EMSCRIPTEN__
static char *self_exe(void) {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return path_abs(buf);
#elif !defined(__EMSCRIPTEN__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; return path_abs(buf); }
#endif
    return NULL;
}

static char *host_path(void) {
    const char *override = getenv("TNY_EXTENSION_HOST");
    if (override && *override) {
        char *p = path_abs(override);
        if (p && regular_file(p)) return p;
        free(p);
        return NULL;
    }

    const char *repo = "python/tny_extension_host.py";
    if (regular_file(repo)) return path_abs(repo);

    char *exe = self_exe();
    if (!exe) return NULL;
    char *bin = path_parent(exe);
    free(exe);
    char *dev_rel = path_join(bin, "../python/tny_extension_host.py");
    char *dev = regular_file(dev_rel) ? path_abs(dev_rel) : NULL;
    free(dev_rel);
    if (dev) { free(bin); return dev; }
    /* Release archives place `tny` beside lib/tny/. */
    char *archive_rel = path_join(bin, "lib/tny/tny_extension_host.py");
    char *archive = regular_file(archive_rel) ? path_abs(archive_rel) : NULL;
    free(archive_rel);
    if (archive) { free(bin); return archive; }
    char *installed_rel = path_join(bin, "../lib/tny/tny_extension_host.py");
    char *installed = regular_file(installed_rel) ? path_abs(installed_rel) : NULL;
    free(installed_rel);
    free(bin);
    return installed;
}

static int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ? -1 : 0;
}
#endif

static void reap_bounded(tny_extensions *x) {
#ifdef __EMSCRIPTEN__
    x->pid = 0;
#else
    if (x->pid <= 0) return;
    int status = 0;
    for (int i = 0; i < 20; i++) {
        pid_t rc = waitpid(x->pid, &status, WNOHANG);
        if (rc == x->pid || rc < 0) { x->pid = 0; return; }
        tny_poll(NULL, 0, 5);
    }
    pid_t pgid = x->pid;
    if (kill(-pgid, SIGTERM) != 0) kill(x->pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        pid_t rc = waitpid(x->pid, &status, WNOHANG);
        if (rc == x->pid || rc < 0) { x->pid = 0; return; }
        tny_poll(NULL, 0, 5);
    }
    if (kill(-pgid, SIGKILL) != 0) kill(x->pid, SIGKILL);
    waitpid(x->pid, &status, 0);
    kill(-pgid, SIGKILL); /* sweep wrapper descendants */
    x->pid = 0;
#endif
}

static void host_stop(tny_extensions *x) {
    if (x->in_fd >= 0) { close(x->in_fd); x->in_fd = -1; }
    reap_bounded(x);
    if (x->out_fd >= 0) { close(x->out_fd); x->out_fd = -1; }
    if (x->err_fd >= 0) { close(x->err_fd); x->err_fd = -1; }
    buf_clear(&x->input);
}

static int host_spawn(tny_extensions *x) {
#ifdef __EMSCRIPTEN__
    set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
               "python extensions are unavailable in wasm");
    return -1;
#else
    char *host = host_path();
    if (!host) {
        set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
                   "python extension host is not installed");
        return -1;
    }
    int inp[2] = {-1, -1}, outp[2] = {-1, -1}, errp[2] = {-1, -1};
    if (pipe(inp) != 0 || pipe(outp) != 0 || pipe(errp) != 0) {
        if (inp[0] >= 0) { close(inp[0]); close(inp[1]); }
        if (outp[0] >= 0) { close(outp[0]); close(outp[1]); }
        if (errp[0] >= 0) { close(errp[0]); close(errp[1]); }
        free(host);
        set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
                   "could not create extension host pipes");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]); free(host);
        set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
                   "could not start extension host");
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        if (chdir(x->cwd) != 0) _exit(127);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        execlp("python3", "python3", "-u", host, (char *)NULL);
        _exit(127);
    }
    free(host);
    setpgid(pid, pid);
    close(inp[0]); close(outp[1]); close(errp[1]);
    x->pid = pid;
    x->in_fd = inp[1];
    x->out_fd = outp[0];
    x->err_fd = errp[0];
    nonblock(x->in_fd);
    nonblock(x->out_fd);
    nonblock(x->err_fd);
    buf_clear(&x->input);
    return 0;
#endif
}

/* Drain without rendering: child text is untrusted and may contain secrets. */
static void drain_stderr(tny_extensions *x) {
    if (x->err_fd < 0) return;
    char discard[4096];
    for (;;) {
        ssize_t n = read(x->err_fd, discard, sizeof discard);
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

/* Pipes raise SIGPIPE before write() can report EPIPE. Block it only on the
 * calling thread, consume the signal generated by this write, then restore the
 * embedder's mask; no process-global signal disposition is changed. */
static ssize_t write_no_sigpipe(int fd, const void *data, size_t len) {
#ifdef __EMSCRIPTEN__
    return write(fd, data, len);
#else
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &one, &old) != 0)
        return write(fd, data, len);
    bool already_blocked = sigismember(&old, SIGPIPE) == 1;
    ssize_t rc = write(fd, data, len);
    if (rc < 0 && errno == EPIPE && !already_blocked) {
        sigset_t pending;
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
            int signo = 0;
            sigwait(&one, &signo);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
#endif
}

static int write_line(tny_extensions *x, const char *json, size_t len,
                      int64_t deadline) {
    if (len > EXT_MAX_LINE || x->in_fd < 0) return -1;
    size_t off = 0;
    while (off <= len) {
        const char *p = off == len ? "\n" : json + off;
        size_t left = off == len ? 1 : len - off;
        ssize_t n = write_no_sigpipe(x->in_fd, p, left);
        if (n > 0) {
            if (off == len) return 0;
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int remaining = (int)(deadline - monotonic_ms());
            if (remaining <= 0) return -2;
            struct pollfd pfd[2] = {{x->in_fd, POLLOUT, 0},
                                    {x->err_fd, POLLIN, 0}};
            if (tny_poll(pfd, x->err_fd >= 0 ? 2 : 1, remaining) <= 0)
                return -2;
            drain_stderr(x);
            continue;
        }
        return -1;
    }
    return 0;
}

/* malloc'd complete response line; NULL on EOF, timeout, or protocol cap. */
static char *read_line(tny_extensions *x, int64_t deadline, int *why) {
    *why = 0;
    for (;;) {
        if (x->input.len) {
            char *nl = memchr(x->input.data, '\n', x->input.len);
            if (nl) {
                size_t n = (size_t)(nl - x->input.data);
                while (n && x->input.data[n - 1] == '\r') n--;
                char *line = xstrndup(x->input.data, n);
                size_t consumed = (size_t)(nl - x->input.data) + 1;
                buf_consume(&x->input, consumed);
                return line;
            }
        }
        if (x->input.len > EXT_MAX_LINE) { *why = 2; return NULL; }
        int remaining = (int)(deadline - monotonic_ms());
        if (remaining <= 0) { *why = 1; return NULL; }
        struct pollfd fds[2] = {{x->out_fd, POLLIN, 0},
                                {x->err_fd, POLLIN, 0}};
        int pr = tny_poll(fds, x->err_fd >= 0 ? 2 : 1, remaining);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { *why = pr == 0 ? 1 : 3; return NULL; }
        drain_stderr(x);
        char tmp[16384];
        for (;;) {
            ssize_t n = read(x->out_fd, tmp, sizeof tmp);
            if (n > 0) {
                if (x->input.len + (size_t)n > EXT_MAX_LINE + 1) {
                    *why = 2; return NULL;
                }
                buf_append(&x->input, tmp, (size_t)n);
                continue;
            }
            if (n == 0) { *why = 3; return NULL; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            *why = 3; return NULL;
        }
    }
}

static yyjson_doc *exchange(tny_extensions *x, const char *json, size_t len,
                            int timeout_ms, int *why) {
    int64_t deadline = monotonic_ms() + timeout_ms;
    int wr = write_line(x, json, len, deadline);
    if (wr != 0) { *why = wr == -2 ? 1 : 3; return NULL; }
    char *line = read_line(x, deadline, why);
    if (!line) return NULL;
    yyjson_doc *doc = jparse(line, strlen(line));
    free(line);
    if (!doc) *why = 2;
    return doc;
}

static int parse_subscriptions(tny_extensions *x, yyjson_val *root) {
    yyjson_val *arr = jget(root, "subscriptions");
    if (!arr || !yyjson_is_arr(arr)) return 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, idx, max, item) {
        const char *event = jget_str(item, "event");
        const char *handler = jget_str(item, "handler_id");
        const char *extension = jget_str(item, "extension");
        if (!event || !*event || !handler || !*handler ||
            strlen(event) > EXT_MAX_EVENT_NAME) continue;
        ext_subscription *next = realloc(
            x->subscriptions, sizeof *next * (x->n_subscriptions + 1));
        if (!next) return -1;
        x->subscriptions = next;
        ext_subscription *sub = &x->subscriptions[x->n_subscriptions++];
        sub->event = dup_cap(event, EXT_MAX_EVENT_NAME);
        sub->handler_id = dup_cap(handler, 160);
        sub->extension = dup_cap(extension ? extension : "extension", 128);
    }
    return 0;
}

static void parse_load_errors(tny_extensions *x, yyjson_val *root) {
    yyjson_val *errors = jget(root, "load_errors");
    if (!errors || !yyjson_is_arr(errors)) return;
    size_t idx, max;
    yyjson_val *error;
    yyjson_arr_foreach(errors, idx, max, error) {
        if (!yyjson_is_obj(error)) continue;
        const char *extension = jget_str(error, "extension");
        const char *kind = jget_str(error, "kind");
        const char *message = jget_str(error, "message");
        pending_failure_add(x, extension, kind, message);
    }
}

static int initialize_host(tny_extensions *x) {
    buf_t req;
    buf_init(&req);
    int64_t id = x->next_id++;
    buf_appendf(&req, "{\"id\":%lld,\"op\":\"initialize\",\"entries\":[",
                (long long)id);
    for (size_t i = 0; i < x->n_entries; i++) {
        if (i) buf_appends(&req, ",");
        jescape(&req, x->entries[i].path);
    }
    buf_appends(&req, "],\"cwd\":");
    jescape(&req, x->cwd);
    buf_appends(&req, "}");
    int why = 0;
    int initialize_timeout = x->timeout_ms < 5000 ? 5000 : x->timeout_ms;
    yyjson_doc *doc = exchange(x, req.data, req.len, initialize_timeout, &why);
    buf_free(&req);
    if (!doc) {
        int status = 0;
        pid_t gone = x->pid > 0 ? waitpid(x->pid, &status, WNOHANG) : -1;
        host_stop(x);
        subscriptions_clear(x);
        if (gone > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127)
            set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
                       "python3 is not installed or cannot run");
        else
            set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
                       why == 1 ? "extension host initialization timed out"
                                : "extension host initialization failed");
        return -1;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool valid = root && yyjson_is_obj(root) &&
                 jget_int(root, "id", -1) == id &&
                 jget_bool(root, "ok", false) &&
                 jget_int(root, "protocol", -1) == EXT_PROTOCOL;
    subscriptions_clear(x);
    if (!valid || parse_subscriptions(x, root) != 0) {
        yyjson_doc_free(doc);
        host_stop(x);
        set_status(x, TNY_EXTENSIONS_UNAVAILABLE,
                   "extension host handshake was invalid");
        return -1;
    }
    parse_load_errors(x, root);
    yyjson_doc_free(doc);
    set_status(x, TNY_EXTENSIONS_READY, "python extensions ready");
    x->unavailable_reported = false;
    return 0;
}

static int ensure_host(tny_extensions *x) {
    if (x->state == TNY_EXTENSIONS_READY && x->pid > 0) return 0;
    if (x->state == TNY_EXTENSIONS_EMPTY ||
        x->state == TNY_EXTENSIONS_UNAVAILABLE) return -1;
    if (host_spawn(x) != 0) return -1;
    return initialize_host(x);
}

static int append_failure(tny_extension_result *out, const char *extension,
                          const char *handler, const char *event, const char *code,
                          const char *message) {
    tny_extension_failure *next = realloc(
        out->failures, sizeof *next * (out->failure_count + 1));
    if (!next) return -1;
    out->failures = next;
    tny_extension_failure *f = &out->failures[out->failure_count++];
    memset(f, 0, sizeof *f);
    f->extension = dup_cap(extension ? extension : "extension", 128);
    f->handler_id = dup_cap(handler ? handler : "", 160);
    f->event = dup_cap(event ? event : "", EXT_MAX_EVENT_NAME);
    f->code = dup_cap(code ? code : "handler_error", 64);
    f->message = dup_cap(message ? message : "extension hook failed", 512);
    return 0;
}

static int drain_pending_failures(tny_extensions *x,
                                  tny_extension_result *out) {
    for (size_t i = 0; i < x->n_pending_failures; i++) {
        tny_extension_failure *failure = &x->pending_failures[i];
        if (append_failure(out, failure->extension, failure->handler_id,
                           failure->event,
                           failure->code, failure->message) != 0)
            return -1;
        free(failure->extension);
        free(failure->handler_id);
        free(failure->event);
        free(failure->code);
        free(failure->message);
    }
    free(x->pending_failures);
    x->pending_failures = NULL;
    x->n_pending_failures = 0;
    return 0;
}

static int append_action(tny_extension_result *out, ext_call *call,
                         yyjson_val *value) {
    if (!value) return 0;
    if (!yyjson_is_obj(value))
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "extension action must be an object");
    const char *type = jget_str(value, "type");
    if (!type) type = jget_str(value, "kind");
    if (!type) type = jget_str(value, "action");
    if (!type)
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "extension action has no kind");
    if (strcmp(type, "none") == 0) return 0;
    tny_extension_action_kind kind;
    if (strcmp(type, "context") == 0) kind = TNY_EXTENSION_ACTION_CONTEXT;
    else if (strcmp(type, "continue") == 0) kind = TNY_EXTENSION_ACTION_CONTINUE;
    else if (strcmp(type, "stop") == 0) kind = TNY_EXTENSION_ACTION_STOP;
    else
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "extension action kind is not supported");
    const char *content = jget_str(value, "content");
    if (kind != TNY_EXTENSION_ACTION_STOP && !content)
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "context and continuation actions need content");
    if (content && strlen(content) > EXT_MAX_ACTION_TEXT)
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "extension action content exceeds the size limit");
    yyjson_val *display_value = jget(value, "display");
    if (display_value && !yyjson_is_bool(display_value))
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "extension action display must be boolean");
    const char *message_kind = jget_str(value, "message_kind");
    if (kind == TNY_EXTENSION_ACTION_CONTINUE && message_kind &&
        strcmp(message_kind, "user") != 0 && strcmp(message_kind, "custom") != 0)
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "continuation message_kind must be user or custom");
    const char *reason = jget_str(value, "reason");
    if (kind == TNY_EXTENSION_ACTION_STOP && !reason)
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "invalid_action",
                              "stop action needs a reason");
    tny_extension_action *next = realloc(
        out->actions, sizeof *next * (out->action_count + 1));
    if (!next) return -1;
    out->actions = next;
    tny_extension_action *a = &out->actions[out->action_count++];
    memset(a, 0, sizeof *a);
    a->kind = kind;
    a->extension = dup_cap(call->extension ? call->extension : "extension", 128);
    a->content = content ? dup_cap(content, EXT_MAX_ACTION_TEXT) : NULL;
    const char *custom = jget_str(value, "custom_type");
    a->custom_type = custom ? dup_cap(custom, 128) : NULL;
    a->reason = reason ? dup_cap(reason, 512) : NULL;
    a->message_kind = message_kind && strcmp(message_kind, "custom") == 0
        ? TNY_EXTENSION_MESSAGE_CUSTOM : TNY_EXTENSION_MESSAGE_USER;
    a->display = jget_bool(value, "display", true);
    return 0;
}

static int parse_invoke_response(tny_extension_result *out, ext_call *call,
                                 int64_t id, yyjson_doc *doc) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root) || jget_int(root, "id", -1) != id)
        return append_failure(out, call->extension, call->handler_id,
                              call->event, "protocol",
                              "extension host returned an invalid response") == 0 ? 1 : -1;

    yyjson_val *failure = jget(root, "failure");
    if (failure && yyjson_is_obj(failure)) {
        const char *code = jget_str(failure, "code");
        const char *message = jget_str(failure, "message");
        if (append_failure(out, call->extension, call->handler_id, call->event,
                           code, message) != 0) return -1;
    } else if (!jget_bool(root, "ok", true)) {
        yyjson_val *error = jget(root, "error");
        const char *code = yyjson_is_obj(error) ? jget_str(error, "kind") : NULL;
        const char *message = yyjson_is_obj(error) ? jget_str(error, "message")
                                                   : jget_str(root, "error");
        if (append_failure(out, call->extension, call->handler_id, call->event,
                           code ? code : "handler_error", message) != 0) return -1;
    }

    yyjson_val *actions = jget(root, "actions");
    if (actions && yyjson_is_arr(actions)) {
        size_t idx, max;
        yyjson_val *action;
        yyjson_arr_foreach(actions, idx, max, action)
            if (append_action(out, call, action) != 0) return -1;
    } else {
        yyjson_val *action = jget(root, "action");
        if (append_action(out, call, action) != 0) return -1;
    }
    return 0;
}

static bool subscription_matches(const ext_subscription *sub,
                                 const char *event) {
    return strcmp(sub->event, "*") == 0 || strcmp(sub->event, event) == 0;
}

static ext_call *matching_calls(tny_extensions *x, const char *event,
                                size_t *count) {
    *count = 0;
    ext_call *calls = NULL;
    for (size_t i = 0; i < x->n_subscriptions; i++) {
        ext_subscription *sub = &x->subscriptions[i];
        if (!subscription_matches(sub, event)) continue;
        ext_call *next = realloc(calls, sizeof *next * (*count + 1));
        if (!next) break;
        calls = next;
        ext_call *call = &calls[(*count)++];
        call->event = xstrdup(sub->event);
        call->handler_id = xstrdup(sub->handler_id);
        call->extension = xstrdup(sub->extension);
    }
    return calls;
}

static void calls_free(ext_call *calls, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(calls[i].event);
        free(calls[i].handler_id);
        free(calls[i].extension);
    }
    free(calls);
}

tny_extensions *tny_extensions_new(const char *tny_dir, const char *cwd,
                                   int handler_timeout_ms) {
    if (!tny_dir || !*tny_dir || !cwd || !*cwd) return NULL;
    tny_extensions *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->tny_dir = xstrdup(tny_dir);
    x->cwd = xstrdup(cwd);
    x->timeout_ms = handler_timeout_ms > 0 ? handler_timeout_ms
                                           : EXT_DEFAULT_TIMEOUT_MS;
    if (x->timeout_ms > EXT_MAX_TIMEOUT_MS) x->timeout_ms = EXT_MAX_TIMEOUT_MS;
    x->in_fd = x->out_fd = x->err_fd = -1;
    x->next_id = 1;
    buf_init(&x->input);
    discover(x);
    if (x->n_entries || x->n_pending_failures)
        set_status(x, TNY_EXTENSIONS_DORMANT, "python extensions discovered");
    else
        set_status(x, TNY_EXTENSIONS_EMPTY, "no python extensions discovered");
    return x;
}

void tny_extensions_free(tny_extensions *x) {
    if (!x) return;
    host_stop(x);
    subscriptions_clear(x);
    for (size_t i = 0; i < x->n_pending_failures; i++) {
        free(x->pending_failures[i].extension);
        free(x->pending_failures[i].handler_id);
        free(x->pending_failures[i].event);
        free(x->pending_failures[i].code);
        free(x->pending_failures[i].message);
    }
    free(x->pending_failures);
    for (size_t i = 0; i < x->n_entries; i++) {
        free(x->entries[i].name);
        free(x->entries[i].path);
    }
    free(x->entries);
    free(x->tny_dir);
    free(x->cwd);
    buf_free(&x->input);
    free(x);
}

tny_extensions_state tny_extensions_get_state(const tny_extensions *x) {
    return x ? x->state : TNY_EXTENSIONS_UNAVAILABLE;
}

size_t tny_extensions_entry_count(const tny_extensions *x) {
    return x ? x->n_entries : 0;
}

const char *tny_extensions_status(const tny_extensions *x) {
    return x ? x->status : "extension manager unavailable";
}

int tny_extensions_invoke(tny_extensions *x, const char *event_name,
                          const char *event_json, tny_extension_result *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!x || !event_name || !*event_name || !event_json ||
        strlen(event_name) > EXT_MAX_EVENT_NAME ||
        strlen(event_json) > EXT_MAX_EVENT_JSON)
        return -1;
    yyjson_doc *event_doc = jparse(event_json, strlen(event_json));
    yyjson_val *event_root = event_doc ? yyjson_doc_get_root(event_doc) : NULL;
    const char *wire_type = event_root && yyjson_is_obj(event_root)
        ? jget_str(event_root, "type") : NULL;
    if (!wire_type || strcmp(wire_type, event_name) != 0) {
        yyjson_doc_free(event_doc);
        return -1;
    }
    yyjson_doc_free(event_doc);
    if (drain_pending_failures(x, out) != 0) return -1;
    if (x->n_entries == 0) {
        set_status(x, TNY_EXTENSIONS_EMPTY, "no python extensions discovered");
        return 0;
    }
    if (x->state == TNY_EXTENSIONS_EMPTY) return 0;
    if (ensure_host(x) != 0) {
        if (x->unavailable_reported) return 0;
        x->unavailable_reported = true;
        return append_failure(out, "extension-host", "", event_name, "unavailable",
                              x->status) == 0 ? 0 : -1;
    }
    if (drain_pending_failures(x, out) != 0) return -1;

    size_t n_calls = 0;
    ext_call *calls = matching_calls(x, event_name, &n_calls);
    for (size_t i = 0; i < n_calls; i++) {
        ext_call *call = &calls[i];
        if (ensure_host(x) != 0) {
            if (append_failure(out, call->extension, call->handler_id, event_name,
                               "unavailable", x->status) != 0) {
                calls_free(calls, n_calls); return -1;
            }
            continue;
        }
        if (drain_pending_failures(x, out) != 0) {
            calls_free(calls, n_calls); return -1;
        }
        buf_t req;
        buf_init(&req);
        int64_t id = x->next_id++;
        buf_appendf(&req, "{\"id\":%lld,\"op\":\"invoke\",\"handler_id\":",
                    (long long)id);
        jescape(&req, call->handler_id);
        buf_appends(&req, ",\"event\":");
        buf_appends(&req, event_json);
        buf_appends(&req, "}");
        int why = 0;
        yyjson_doc *response = exchange(x, req.data, req.len, x->timeout_ms, &why);
        buf_free(&req);
        if (!response) {
            const char *code = why == 1 ? "timeout" : "host_failure";
            const char *message = why == 1 ? "extension handler timed out"
                                            : why == 2
                                                ? "extension host response exceeded limits or was invalid"
                                                : "extension host stopped during handler invocation";
            if (append_failure(out, call->extension, call->handler_id, event_name,
                               code, message) != 0) {
                calls_free(calls, n_calls); return -1;
            }
            host_stop(x);
            subscriptions_clear(x);
            set_status(x, TNY_EXTENSIONS_DORMANT,
                       "python extension host will restart on demand");
            if (append_failure(out, "extension-host", "", event_name,
                               "host_state_reset",
                               "extension process state was reset after host failure") != 0) {
                calls_free(calls, n_calls); return -1;
            }
            continue; /* fail open; later handlers get a fresh host */
        }
        int rc = parse_invoke_response(out, call, id, response);
        yyjson_doc_free(response);
        if (rc < 0) { calls_free(calls, n_calls); return -1; }
        if (rc > 0) {
            host_stop(x);
            subscriptions_clear(x);
            set_status(x, TNY_EXTENSIONS_DORMANT,
                       "python extension host will restart on demand");
        }
    }
    calls_free(calls, n_calls);
    return 0;
}

void tny_extension_result_free(tny_extension_result *result) {
    if (!result) return;
    for (size_t i = 0; i < result->action_count; i++) {
        free(result->actions[i].extension);
        free(result->actions[i].content);
        free(result->actions[i].custom_type);
        free(result->actions[i].reason);
    }
    for (size_t i = 0; i < result->failure_count; i++) {
        free(result->failures[i].extension);
        free(result->failures[i].handler_id);
        free(result->failures[i].event);
        free(result->failures[i].code);
        free(result->failures[i].message);
    }
    free(result->actions);
    free(result->failures);
    memset(result, 0, sizeof *result);
}
