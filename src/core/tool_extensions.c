/* tool_extensions.c — discovery, catalog cache and the tool host client for
 * Python tool extensions (docs/extensions.md, ADR 0218). */
#include "core/tool_extensions.h"

#include "core/tool_host.h"
#include "core/tools.h"
#include "json/json.h"
#include "util/process.h"
#include "util/tny_poll.h"
#include "util/util.h"
#include "tny_version.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define EXT_SUBDIR    "extensions/tools"
#define WALK_DEPTH    8
#define LINE_MAX_SIZE (64u * 1024u * 1024u)

typedef struct {
    pthread_mutex_t lock;
    char key[17];        /* fingerprint of the loaded catalog */
    yyjson_doc *catalog; /* {"tools":[...],...} for `key` */
    char host_key[17];   /* fingerprint the running host was started for */
    pid_t pid;
    int to_host;
    int from_host;
    buf_t pending; /* bytes read past the last full line */
    long next_id;
    int stderr_fd;
} ext_state;

static ext_state g_ext = {
    .lock = PTHREAD_MUTEX_INITIALIZER, .pid = -1, .to_host = -1, .from_host = -1, .stderr_fd = -1};

bool tny_tool_ext_enabled(const tny_ctx *ctx) {
#ifdef __EMSCRIPTEN__
    (void)ctx;
    return false;
#else
    if (!ctx || ctx->tool_extensions_disabled || ctx->library_mode || ctx->prompt_optimisation)
        return false;
    return true;
#endif
}

void tny_tool_ext_dirs(const tny_ctx *ctx, char **project_dir, char **global_dir) {
    *project_dir = NULL;
    *global_dir = NULL;
    if (!ctx) return;
    char *global = ctx->tny_dir ? path_join(ctx->tny_dir, EXT_SUBDIR) : NULL;
    char *project = NULL;
    if (!ctx->project_tools_disabled && ctx->cwd) project = path_join(ctx->cwd, ".tny/" EXT_SUBDIR);
    if (project && global) {
        char *a = path_abs(project), *b = path_abs(global);
        bool same = a && b && strcmp(a, b) == 0;
        free(a);
        free(b);
        if (same) {
            free(project);
            project = NULL;
        }
    }
    *project_dir = project;
    *global_dir = global;
}

/* ---- fingerprint -------------------------------------------------------- */

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char **sorted_entries(const char *dir, size_t *n_out) {
    *n_out = 0;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "__pycache__") == 0) continue;
        if (n == cap) {
            size_t next = cap ? cap * 2 : 16;
            char **grown = realloc(names, next * sizeof *names);
            if (!grown) break;
            names = grown;
            cap = next;
        }
        names[n] = xstrdup(e->d_name);
        if (names[n]) n++;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, cmp_names);
    *n_out = n;
    return names;
}

static void free_names(char **names, size_t n) {
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

static void stat_line(buf_t *b, const char *path, const struct stat *st) {
#if defined(__APPLE__)
    long nsec = st->st_mtimespec.tv_nsec;
#else
    long nsec = st->st_mtim.tv_nsec;
#endif
    buf_appendf(b, "%s %lld %lld.%ld %llu\n", path, (long long)st->st_size, (long long)st->st_mtime,
                nsec, (unsigned long long)st->st_ino);
}

/* Every .py file under a folder module: a change anywhere in it can change
 * which tools the module declares. */
static void walk_package(buf_t *b, const char *dir, int depth) {
    if (depth > WALK_DEPTH) return;
    size_t n;
    char **names = sorted_entries(dir, &n);
    for (size_t i = 0; i < n; i++) {
        char *path = path_join(dir, names[i]);
        struct stat st;
        if (path && stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) walk_package(b, path, depth + 1);
            else if (S_ISREG(st.st_mode) && str_ends(names[i], ".py")) stat_line(b, path, &st);
        }
        free(path);
    }
    free_names(names, n);
}

static void fingerprint_dir(buf_t *b, const char *scope, const char *dir) {
    buf_appendf(b, "[%s] %s\n", scope, dir ? dir : "-");
    if (!dir) return;
    size_t n;
    char **names = sorted_entries(dir, &n);
    for (size_t i = 0; i < n; i++) {
        if (names[i][0] == '_') continue;
        char *path = path_join(dir, names[i]);
        struct stat st;
        if (path && stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                buf_appendf(b, "dir %s\n", names[i]);
                walk_package(b, path, 1);
            } else if (S_ISREG(st.st_mode) && str_ends(names[i], ".py")) {
                stat_line(b, path, &st);
            }
        }
        free(path);
    }
    free_names(names, n);
}

/* Returns false when no extension directory exists (nothing to load). */
static bool fingerprint(const tny_ctx *ctx, char out[17], char **project, char **global) {
    tny_tool_ext_dirs(ctx, project, global);
    bool any = (*project && dir_exists(*project)) || (*global && dir_exists(*global));
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "tny %s %016llx\n", TNY_VERSION,
                (unsigned long long)tny_tool_host_package_hash());
    fingerprint_dir(&b, "project", *project && dir_exists(*project) ? *project : NULL);
    fingerprint_dir(&b, "global", *global && dir_exists(*global) ? *global : NULL);
    snprintf(out, 17, "%016llx", (unsigned long long)fnv1a(b.data ? b.data : "", b.len));
    buf_free(&b);
    return any;
}

/* ---- host process ------------------------------------------------------- */

static void host_stop_locked(void) {
    if (g_ext.to_host >= 0) close(g_ext.to_host);
    if (g_ext.from_host >= 0) close(g_ext.from_host);
    g_ext.to_host = g_ext.from_host = -1;
    if (g_ext.pid > 0) {
        tny_process_kill_tree(g_ext.pid);
        int status;
        while (waitpid(g_ext.pid, &status, 0) < 0 && errno == EINTR) {}
    }
    g_ext.pid = -1;
    g_ext.host_key[0] = '\0';
    buf_free(&g_ext.pending);
}

/* Process exit: a host blocked inside a tool would otherwise outlive us. */
static void ext_atexit(void) {
    if (g_ext.pid > 0) tny_process_kill_tree(g_ext.pid);
}

static int open_log(const tny_ctx *ctx) {
    if (!ctx->tny_dir) return -1;
    char *dir = path_join(ctx->tny_dir, "logs");
    char *file = dir ? path_join(dir, "tool-extensions.log") : NULL;
    int fd = -1;
    if (file && mkdir_p(dir) == 0) fd = open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    free(dir);
    free(file);
    return fd;
}

static int host_start_locked(const tny_ctx *ctx, const char *key, const char *project,
                             const char *global, char *why, size_t why_len) {
    host_stop_locked();
    char *self = tny_process_self_path();
    if (!self) {
        snprintf(why, why_len, "cannot locate the tny executable");
        return -1;
    }
    char *argv[8];
    int argc = 0;
    argv[argc++] = self;
    argv[argc++] = (char *)"--tool-host";
    argv[argc++] = (char *)"serve";
    if (project && dir_exists(project)) {
        argv[argc++] = (char *)"--project";
        argv[argc++] = (char *)project;
    }
    if (global && dir_exists(global)) {
        argv[argc++] = (char *)"--global";
        argv[argc++] = (char *)global;
    }
    argv[argc] = NULL;
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0) {
        free(self);
        snprintf(why, why_len, "pipe: %s", strerror(errno));
        return -1;
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        free(self);
        snprintf(why, why_len, "pipe: %s", strerror(errno));
        return -1;
    }
    int err_fd = g_ext.stderr_fd >= 0 ? g_ext.stderr_fd : open_log(ctx);
    tny_fd_mapping maps[3] = {{in_pipe[0], 0}, {out_pipe[1], 1}, {err_fd, 2}};
    pid_t pid = -1;
    int rc = tny_process_spawn_mapped(argv, environ, maps, err_fd >= 0 ? 3 : 2, &pid);
    if (err_fd >= 0 && err_fd != g_ext.stderr_fd) close(err_fd);
    close(in_pipe[0]);
    close(out_pipe[1]);
    free(self);
    if (rc != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        snprintf(why, why_len, "cannot start the tool host: %s", strerror(rc));
        return -1;
    }
    (void)fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    static bool registered;
    if (!registered) registered = atexit(ext_atexit) == 0;
    g_ext.pid = pid;
    g_ext.to_host = in_pipe[1];
    g_ext.from_host = out_pipe[0];
    buf_init(&g_ext.pending);
    snprintf(g_ext.host_key, sizeof g_ext.host_key, "%s", key);
    return 0;
}

static int write_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t w = write(fd, data, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += w;
        len -= (size_t)w;
    }
    return 0;
}

/* One response line for request `id`. Lines with another id (or none) are
 * skipped. Returns the parsed message, or NULL with `why` set. */
static yyjson_doc *host_request_locked(const char *method, const char *params_json, int timeout_ms,
                                       bool (*cancelled)(void *), void *ud, char *why,
                                       size_t why_len) {
    long id = ++g_ext.next_id;
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\",\"params\":%s}\n", id,
                method, params_json ? params_json : "{}");
    if (buf_oom(&req)) {
        buf_free(&req);
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    struct sigaction ignore = {0}, old;
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore, &old);
    int wrc = write_all(g_ext.to_host, req.data, req.len);
    sigaction(SIGPIPE, &old, NULL);
    buf_free(&req);
    if (wrc != 0) {
        snprintf(why, why_len, "the tool host exited (see ~/.tny/logs/tool-extensions.log)");
        return NULL;
    }
    int64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        char *nl = g_ext.pending.len ? memchr(g_ext.pending.data, '\n', g_ext.pending.len) : NULL;
        if (nl) {
            size_t line_len = (size_t)(nl - g_ext.pending.data);
            yyjson_doc *doc = jparse(g_ext.pending.data, line_len);
            buf_consume(&g_ext.pending, line_len + 1);
            yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
            yyjson_val *rid = jget(root, "id");
            if (rid && yyjson_is_int(rid) && yyjson_get_sint(rid) == id) return doc;
            yyjson_doc_free(doc);
            continue;
        }
        if (cancelled && cancelled(ud)) {
            snprintf(why, why_len, "cancelled");
            return NULL;
        }
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) {
            snprintf(why, why_len, "timed out after %ds", timeout_ms / 1000);
            return NULL;
        }
        struct pollfd pfd = {.fd = g_ext.from_host, .events = POLLIN};
        int wait = left > 100 ? 100 : (int)left;
        int pr = tny_poll(&pfd, 1, wait);
        if (pr < 0 && errno != EINTR) {
            snprintf(why, why_len, "poll: %s", strerror(errno));
            return NULL;
        }
        if (pr <= 0) continue;
        char chunk[65536];
        /* Calls are serialised on the one host by design; poll above bounds
         * the wait. NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) */
        ssize_t r = read(g_ext.from_host, chunk, sizeof chunk);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            snprintf(why, why_len, "the tool host exited (see ~/.tny/logs/tool-extensions.log)");
            return NULL;
        }
        buf_append(&g_ext.pending, chunk, (size_t)r);
        if (buf_oom(&g_ext.pending) || g_ext.pending.len > LINE_MAX_SIZE) {
            snprintf(why, why_len, "the tool host sent an oversized reply");
            return NULL;
        }
    }
}

static int host_ensure_locked(const tny_ctx *ctx, const char *key, const char *project,
                              const char *global, char *why, size_t why_len) {
    if (g_ext.pid > 0 && strcmp(g_ext.host_key, key) == 0) return 0;
    if (host_start_locked(ctx, key, project, global, why, why_len) != 0) return -1;
    yyjson_doc *init =
        host_request_locked("initialize",
                            "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                            "\"clientInfo\":{\"name\":\"tny\",\"version\":\"" TNY_VERSION "\"}}",
                            60000, NULL, NULL, why, why_len);
    if (!init) {
        host_stop_locked();
        return -1;
    }
    yyjson_doc_free(init);
    return 0;
}

/* ---- catalog ------------------------------------------------------------ */

static char *cache_path(const tny_ctx *ctx, const char *key) {
    if (!ctx->tny_dir) return NULL;
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s/cache/tool-extensions/%s.json", ctx->tny_dir, key);
    return buf_detach(&b);
}

static yyjson_doc *error_catalog(const char *why) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"tools\":[],\"warnings\":[],\"errors\":[{\"error\":");
    jescape(&b, why);
    buf_appends(&b, "}]}");
    yyjson_doc *doc = b.data ? jparse(b.data, b.len) : NULL;
    buf_free(&b);
    return doc;
}

/* A catalog is a pure function of the fingerprinted files unless a proxy
 * failed to list its server, or the host itself failed: those may be
 * transient, so they are never cached. */
static bool catalog_cacheable(yyjson_doc *doc) {
    yyjson_val *errors = jget(yyjson_doc_get_root(doc), "errors");
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(errors, idx, max, e) {
        const char *msg = jget_str(e, "error");
        if (!jget_str(e, "module") || (msg && str_starts(msg, "proxy:"))) return false;
    }
    return true;
}

/* Drop catalogs nobody has written for a week (old module versions). */
static void prune_cache(const char *dir, const char *keep) {
    size_t n;
    char **names = sorted_entries(dir, &n);
    time_t cutoff = time(NULL) - 7 * 24 * 3600;
    for (size_t i = 0; i < n; i++) {
        char *path = path_join(dir, names[i]);
        struct stat st;
        if (path && strcmp(path, keep) != 0 && str_ends(names[i], ".json") &&
            stat(path, &st) == 0 && st.st_mtime < cutoff)
            (void)unlink(path);
        free(path);
    }
    free_names(names, n);
}

/* The catalog for the current fingerprint, loading it under the lock. */
static yyjson_doc *catalog_locked(tny_ctx *ctx, bool refresh) {
    char key[17], *project = NULL, *global = NULL;
    bool any = fingerprint(ctx, key, &project, &global);
    if (!refresh && g_ext.catalog && strcmp(g_ext.key, key) == 0) {
        free(project);
        free(global);
        return g_ext.catalog;
    }
    yyjson_doc_free(g_ext.catalog);
    g_ext.catalog = NULL;
    snprintf(g_ext.key, sizeof g_ext.key, "%s", key);
    if (!any) {
        g_ext.catalog = jparse("{\"tools\":[],\"errors\":[],\"warnings\":[]}", 39);
        free(project);
        free(global);
        return g_ext.catalog;
    }
    char *cache = cache_path(ctx, key);
    if (!refresh && cache) g_ext.catalog = jparse_file(cache);
    if (g_ext.catalog && !yyjson_is_arr(jget(yyjson_doc_get_root(g_ext.catalog), "tools"))) {
        yyjson_doc_free(g_ext.catalog);
        g_ext.catalog = NULL;
    }
    if (!g_ext.catalog) {
        char why[512] = "";
        yyjson_doc *reply = NULL;
        if (host_ensure_locked(ctx, key, project, global, why, sizeof why) == 0)
            reply = host_request_locked("tny/catalog", "{}", 120000, NULL, NULL, why, sizeof why);
        if (!reply) host_stop_locked();
        yyjson_val *result = reply ? jget(yyjson_doc_get_root(reply), "result") : NULL;
        char *text = result ? jwrite_val(result) : NULL;
        yyjson_doc_free(reply);
        if (text) g_ext.catalog = jparse(text, strlen(text));
        if (text && g_ext.catalog && catalog_cacheable(g_ext.catalog) && cache) {
            char *dir = path_join(ctx->tny_dir, "cache/tool-extensions");
            if (dir && mkdir_p(dir) == 0 && file_write_atomic(cache, text, strlen(text)) == 0)
                prune_cache(dir, cache);
            free(dir);
        }
        free(text);
        if (!g_ext.catalog) g_ext.catalog = error_catalog(why[0] ? why : "the tool host failed");
    }
    free(cache);
    free(project);
    free(global);
    return g_ext.catalog;
}

char *tny_tool_ext_report(tny_ctx *ctx, bool refresh) {
    if (!tny_tool_ext_enabled(ctx)) return NULL;
    pthread_mutex_lock(&g_ext.lock);
    yyjson_doc *doc = catalog_locked(ctx, refresh);
    char *json = doc ? jwrite_val(yyjson_doc_get_root(doc)) : NULL;
    pthread_mutex_unlock(&g_ext.lock);
    return json;
}

char *tny_tool_ext_report_cached(tny_ctx *ctx) {
    if (!tny_tool_ext_enabled(ctx)) return NULL;
    pthread_mutex_lock(&g_ext.lock);
    char key[17], *project = NULL, *global = NULL;
    char *json = NULL;
    if (fingerprint(ctx, key, &project, &global)) {
        if (g_ext.catalog && strcmp(g_ext.key, key) == 0) {
            json = jwrite_val(yyjson_doc_get_root(g_ext.catalog));
        } else {
            char *cache = cache_path(ctx, key);
            yyjson_doc *doc = cache ? jparse_file(cache) : NULL;
            yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
            if (yyjson_is_arr(jget(root, "tools"))) json = jwrite_val(root);
            yyjson_doc_free(doc);
            free(cache);
        }
    }
    pthread_mutex_unlock(&g_ext.lock);
    free(project);
    free(global);
    return json;
}

/* The host's NAME_RE: a letter or `_`, then letters, digits, `_` or `-`. */
static bool module_name_ok(const char *name, size_t len) {
    if (len == 0 || len > 64 || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (size_t i = 1; i < len; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') return false;
    return true;
}

static void add_module_names(const char *dir, char ***names, size_t *n) {
    size_t count;
    char **entries = dir ? sorted_entries(dir, &count) : NULL;
    for (size_t i = 0; entries && i < count; i++) {
        const char *e = entries[i];
        if (e[0] == '_') continue;
        char *path = path_join(dir, e);
        struct stat st;
        size_t len = strlen(e);
        bool ok = false;
        if (path && stat(path, &st) == 0) {
            if (S_ISREG(st.st_mode) && str_ends(e, ".py")) {
                len -= 3;
                ok = true;
            } else if (S_ISDIR(st.st_mode)) {
                char *entry = path_join(path, "__main__.py");
                ok = entry && stat(entry, &st) == 0 && S_ISREG(st.st_mode);
                free(entry);
            }
        }
        free(path);
        if (!ok || !module_name_ok(e, len)) continue;
        bool seen = false;
        for (size_t j = 0; j < *n && !seen; j++)
            seen = strlen((*names)[j]) == len && strncmp((*names)[j], e, len) == 0;
        if (seen) continue;
        char *name = malloc(len + 1);
        char **grown = name ? realloc(*names, (*n + 1) * sizeof **names) : NULL;
        if (!grown) {
            free(name);
            break;
        }
        memcpy(name, e, len);
        name[len] = '\0';
        *names = grown;
        (*names)[(*n)++] = name;
    }
    free_names(entries, entries ? count : 0);
}

char **tny_tool_ext_module_names(const tny_ctx *ctx, size_t *count) {
    *count = 0;
    if (!tny_tool_ext_enabled(ctx)) return NULL;
    char *project = NULL, *global = NULL;
    tny_tool_ext_dirs(ctx, &project, &global);
    char **names = NULL;
    add_module_names(project, &names, count);
    add_module_names(global, &names, count);
    free(project);
    free(global);
    if (*count > 1) qsort(names, *count, sizeof *names, cmp_names);
    return names;
}

void tny_tool_ext_names_free(char **names, size_t count) { free_names(names, count); }

static yyjson_val *find_tool(yyjson_doc *doc, const char *name) {
    yyjson_val *tools = jget(doc ? yyjson_doc_get_root(doc) : NULL, "tools");
    size_t idx, max;
    yyjson_val *t;
    yyjson_arr_foreach(tools, idx, max, t) {
        const char *n = jget_str(t, "name");
        if (n && strcmp(n, name) == 0) return t;
    }
    return NULL;
}

char *tny_tool_ext_functions_json(tny_ctx *ctx, bool (*skip)(const char *, void *), void *ud) {
    if (!tny_tool_ext_enabled(ctx)) return xstrdup("[]");
    pthread_mutex_lock(&g_ext.lock);
    yyjson_doc *doc = catalog_locked(ctx, false);
    yyjson_val *tools = jget(doc ? yyjson_doc_get_root(doc) : NULL, "tools");
    yyjson_mut_doc *out = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *arr = out ? yyjson_mut_arr(out) : NULL;
    char *json = NULL;
    if (arr) {
        size_t idx, max;
        yyjson_val *t;
        yyjson_arr_foreach(tools, idx, max, t) {
            const char *name = jget_str(t, "name");
            if (!name || (skip && skip(name, ud))) continue;
            yyjson_mut_val *item = yyjson_mut_obj(out);
            yyjson_mut_val *fn = yyjson_mut_obj(out);
            yyjson_mut_obj_add_str(out, item, "type", "function");
            yyjson_mut_obj_add_strcpy(out, fn, "name", name);
            const char *desc = jget_str(t, "description");
            yyjson_mut_obj_add_strcpy(out, fn, "description", desc ? desc : "");
            yyjson_val *schema = jget(t, "inputSchema");
            yyjson_mut_obj_add_val(out, fn, "parameters",
                                   schema ? yyjson_val_mut_copy(out, schema) : yyjson_mut_obj(out));
            yyjson_mut_obj_add_val(out, item, "function", fn);
            yyjson_mut_arr_add_val(arr, item);
        }
        yyjson_mut_doc_set_root(out, arr);
        json = jwrite(out);
    }
    yyjson_mut_doc_free(out);
    pthread_mutex_unlock(&g_ext.lock);
    return json;
}

char *tny_tool_ext_schema(tny_ctx *ctx, const char *name) {
    if (!name || !tny_tool_ext_enabled(ctx)) return NULL;
    pthread_mutex_lock(&g_ext.lock);
    yyjson_val *t = find_tool(catalog_locked(ctx, false), name);
    yyjson_val *schema = jget(t, "inputSchema");
    char *json = t ? (schema ? jwrite_val(schema) : xstrdup("{\"type\":\"object\"}")) : NULL;
    pthread_mutex_unlock(&g_ext.lock);
    return json;
}

static int tool_timeout_ms(yyjson_val *tool) {
    const char *env = getenv("TNY_TOOL_TIMEOUT_MS");
    int ms = TNY_TOOL_EXT_DEFAULT_TIMEOUT_MS;
    if (env && *env) {
        long v = strtol(env, NULL, 10);
        if (v > 0 && v < 86400000L) ms = (int)v;
    }
    double seconds = jget_num(tool, "timeout", 0);
    if (seconds > 0 && seconds < 86400) ms = (int)(seconds * 1000);
    return ms;
}

char *tny_tool_ext_call(tny_ctx *ctx, const char *name, const char *args_json,
                        bool (*cancelled)(void *), void *ud, bool *is_error) {
    *is_error = true;
    if (!tny_tool_ext_enabled(ctx)) return xstrdup("tool extensions are disabled");
    pthread_mutex_lock(&g_ext.lock);
    char why[512] = "";
    char *out = NULL;
    yyjson_doc *doc = catalog_locked(ctx, false);
    yyjson_val *tool = find_tool(doc, name);
    if (!tool) {
        out = tool_err("unknown tool %s", name);
        goto done;
    }
    int timeout_ms = tool_timeout_ms(tool);
    char *project = NULL, *global = NULL;
    tny_tool_ext_dirs(ctx, &project, &global);
    int rc = host_ensure_locked(ctx, g_ext.key, project, global, why, sizeof why);
    free(project);
    free(global);
    if (rc != 0) {
        out = tool_err("%s: %s", name, why);
        goto done;
    }
    buf_t params;
    buf_init(&params);
    buf_appends(&params, "{\"name\":");
    jescape(&params, name);
    buf_appendf(&params, ",\"arguments\":%s}", args_json && *args_json ? args_json : "{}");
    char *params_json = buf_detach(&params);
    yyjson_doc *reply = params_json ? host_request_locked("tools/call", params_json, timeout_ms,
                                                          cancelled, ud, why, sizeof why)
                                    : NULL;
    free(params_json);
    if (!reply) {
        host_stop_locked(); /* a hung or cancelled tool takes the host down */
        out = tool_err("%s %s", name, why[0] ? why : "failed");
        goto done;
    }
    yyjson_val *root = yyjson_doc_get_root(reply);
    yyjson_val *result = jget(root, "result");
    if (!result) {
        const char *msg = jget_str(jget(root, "error"), "message");
        out = tool_err("%s: %s", name, msg ? msg : "tool host error");
    } else {
        buf_t text;
        buf_init(&text);
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(jget(result, "content"), idx, max, item) {
            const char *s = jget_str(item, "text");
            if (!s) continue;
            if (text.len) buf_appends(&text, "\n");
            buf_appends(&text, s);
        }
        *is_error = jget_bool(result, "isError", false);
        out = buf_detach(&text);
        if (!out) out = xstrdup("");
    }
    yyjson_doc_free(reply);
done:
    pthread_mutex_unlock(&g_ext.lock);
    return out;
}

void tny_tool_ext_set_stderr(int fd) {
    pthread_mutex_lock(&g_ext.lock);
    g_ext.stderr_fd = fd;
    pthread_mutex_unlock(&g_ext.lock);
}

void tny_tool_ext_shutdown(void) {
    pthread_mutex_lock(&g_ext.lock);
    host_stop_locked();
    yyjson_doc_free(g_ext.catalog);
    g_ext.catalog = NULL;
    g_ext.key[0] = '\0';
    pthread_mutex_unlock(&g_ext.lock);
}
