/* mcp.c — stdio JSONL MCP client. Server output is untrusted data.
 * Profile: ~/.tny/mcp.json {"servers":{"<name>":{"command":["…"]}}}.
 * Optional mcp.import_from (docs/adr/0051) merges the named harness's user
 * and documented project configs; no foreign file is read without opt-in.
 *
 * Startup (docs/adr/0049): a native session calls mcp_warm_start once, off
 * the event loop — one detached thread per profile server runs spawn +
 * initialize + tools/list and commits the connection under g_mu, so the
 * first prompt already has the tool catalog. A tool call that names a
 * server mid-warm waits on the condvar (the same cost the lazy path paid);
 * nothing else ever blocks. A failed warm-up stays silent until a call
 * names that server, which retries the spawn and reports the usual error.
 * Without threads (wasm) every slot stays lazy and calls keep today's
 * clean spawn error. */
#include "mcp/mcp.h"
#include "mcp/mcp_import.h"
#include "util/util.h"
#include "util/tny_poll.h"

#include <ctype.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MCP_MAX_SERVERS 16
#define MCP_TIMEOUT_MS  30000
/* catalog caps (docs/adr/0049): one line per tool, a bounded list per
 * session; overflow is discoverable through mcp_search_tools */
#define MCP_CATALOG_DESC_MAX  120
#define MCP_CATALOG_MAX_TOOLS 64

/* One live stdio connection. Owned by exactly one thread at a time: the
 * warm thread until its commit under g_mu, the calling thread after. */
typedef struct {
    pid_t pid;
    int in_fd;  /* write requests here */
    int out_fd; /* read responses here */
    buf_t rbuf;
    int next_id;
    yyjson_doc *tools; /* cached tools/list result */
} mcp_conn;

typedef enum {
    SRV_EMPTY = 0, /* free slot */
    SRV_WARMING,   /* background handshake in flight */
    SRV_READY,     /* conn usable from the calling thread */
    SRV_FAILED     /* warm-up failed; a call retries and reports */
} mcp_state;

typedef struct {
    char *name;
    mcp_state state;
    bool abandoned; /* shutdown while warming: the warm thread cleans up */
    mcp_conn conn;
} mcp_server;

static mcp_server g_servers[MCP_MAX_SERVERS];
static int g_nservers = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_warm_started = false;

static void conn_close(mcp_conn *c) {
    if (c->pid > 0) {
        close(c->in_fd);
        close(c->out_fd);
        kill(c->pid, SIGTERM);
        waitpid(c->pid, NULL, WNOHANG);
    }
    buf_free(&c->rbuf);
    yyjson_doc_free(c->tools);
    memset(c, 0, sizeof *c);
}

void mcp_shutdown_all(void) {
    pthread_mutex_lock(&g_mu);
    bool warming_left = false;
    for (int i = 0; i < g_nservers; i++) {
        mcp_server *s = &g_servers[i];
        if (s->state == SRV_WARMING) {
            /* The warm thread owns the conn: it kills and frees on commit.
             * If the process exits first, the child's stdin closes and a
             * well-behaved server exits on EOF. */
            s->abandoned = true;
            warming_left = true;
            continue;
        }
        conn_close(&s->conn);
        free(s->name);
        memset(s, 0, sizeof *s);
    }
    if (!warming_left) g_nservers = 0;
    g_warm_started = false; /* a later session in this process may warm again */
    pthread_mutex_unlock(&g_mu);
}

/* Send one JSON-RPC request and wait for the matching id. Returns doc. */
static yyjson_doc *rpc(mcp_conn *c, const char *method, const char *params_json) {
    int id = ++c->next_id;
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}\n", id,
                method, params_json ? params_json : "{}");
    ssize_t w = write(c->in_fd, req.data, req.len);
    buf_free(&req);
    if (w < 0) return NULL;
    int64_t deadline = now_ms() + MCP_TIMEOUT_MS;
    for (;;) {
        /* scan buffered lines */
        for (;;) {
            char *nl = c->rbuf.len ? memchr(c->rbuf.data, '\n', c->rbuf.len) : NULL;
            if (!nl) break;
            size_t ll = (size_t)(nl - c->rbuf.data);
            yyjson_doc *doc = yyjson_read(c->rbuf.data, ll, 0);
            buf_consume(&c->rbuf, ll + 1);
            if (!doc) continue;
            yyjson_val *r = yyjson_doc_get_root(doc);
            if ((int)jget_int(r, "id", -1) == id) return doc;
            yyjson_doc_free(doc); /* notification or stale — ignore */
        }
        int left = (int)(deadline - now_ms());
        if (left <= 0) return NULL;
        struct pollfd pf = {c->out_fd, POLLIN, 0};
        if (tny_poll(&pf, 1, left) <= 0) return NULL;
        char tmp[16384];
        ssize_t n = read(c->out_fd, tmp, sizeof tmp);
        if (n <= 0) return NULL;
        buf_append(&c->rbuf, tmp, (size_t)n);
    }
}

static void notify(mcp_conn *c, const char *method) {
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}\n", method);
    ssize_t w = write(c->in_fd, req.data, req.len);
    (void)w;
    buf_free(&req);
}

static yyjson_doc *conn_tools(mcp_conn *c) {
    if (!c->tools) c->tools = rpc(c, "tools/list", "{}");
    return c->tools;
}

extern char **environ;

/* Build the child environment before fork(): warm-up threads fork
 * concurrently, so the child may only use async-signal-safe calls — setenv()
 * there can deadlock on the allocator lock. Entries at index >= *owned_from
 * are malloc'd; the rest borrow the parent's environ strings. */
static char **build_envp(const mcp_imported_server *srv, size_t *owned_from) {
    size_t base = 0;
    while (environ[base]) base++;
    char **envp = calloc(base + (size_t)srv->nenv + 1, sizeof *envp);
    if (!envp) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < base; i++) {
        const char *e = environ[i];
        const char *eq = strchr(e, '=');
        size_t klen = eq ? (size_t)(eq - e) : strlen(e);
        bool overridden = false;
        for (int j = 0; j < srv->nenv; j++) {
            const char *k = srv->env[j].key;
            if (k && strlen(k) == klen && memcmp(k, e, klen) == 0) overridden = true;
        }
        if (!overridden) envp[n++] = (char *)e;
    }
    *owned_from = n;
    for (int j = 0; j < srv->nenv; j++) {
        if (!srv->env[j].key || !srv->env[j].key[0]) continue;
        const char *val = srv->env[j].value ? srv->env[j].value : "";
        size_t len = strlen(srv->env[j].key) + strlen(val) + 2;
        char *kv = malloc(len);
        if (!kv) {
            for (size_t i = *owned_from; i < n; i++) free(envp[i]);
            free(envp);
            return NULL;
        }
        snprintf(kv, len, "%s=%s", srv->env[j].key, val);
        envp[n++] = kv;
    }
    return envp;
}

static void free_envp(char **envp, size_t owned_from) {
    if (!envp) return;
    for (size_t i = owned_from; envp[i]; i++) secure_free(envp[i]);
    free(envp);
}

/* Spawn one server process and run the initialize handshake. Extra env
 * (from a foreign harness profile) is applied only in the child. */
static int conn_open(mcp_conn *c, char *const argv[], const char *cwd,
                     const mcp_imported_server *srv) {
    memset(c, 0, sizeof *c);
    buf_init(&c->rbuf);
    char **child_env = NULL;
    size_t child_env_owned = 0;
    if (srv && srv->nenv > 0) {
        child_env = build_envp(srv, &child_env_owned);
        if (!child_env) return -1;
    }
    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) {
        free_envp(child_env, child_env_owned);
        return -1;
    }
    if (pipe(outpipe) != 0) {
        free_envp(child_env, child_env_owned);
        close(inpipe[0]);
        close(inpipe[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        free_envp(child_env, child_env_owned);
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(inpipe[0], 0);
        dup2(outpipe[1], 1);
        /* stderr inherited: logs visible, protocol clean */
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        /* pointer store only — the child must stay async-signal-safe */
        if (child_env) environ = child_env;
        if (chdir(cwd) != 0) _exit(127);
        if (!argv[0]) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    free_envp(child_env, child_env_owned);
    close(inpipe[0]);
    close(outpipe[1]);
    c->pid = pid;
    c->in_fd = inpipe[1];
    c->out_fd = outpipe[0];

    yyjson_doc *init = rpc(c, "initialize",
                           "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                           "\"clientInfo\":{\"name\":\"tny\",\"version\":\"" TNY_VERSION "\"}}");
    if (!init) {
        conn_close(c);
        return -1;
    }
    yyjson_doc_free(init);
    notify(c, "notifications/initialized");
    return 0;
}

/* malloc'd NULL-terminated argv copy from a catalog entry, or NULL. */
static char **argv_from_server(const mcp_imported_server *srv) {
    if (!srv || srv->skipped || srv->argc <= 0) return NULL;
    char **argv = calloc((size_t)srv->argc + 1, sizeof *argv);
    if (!argv) return NULL;
    for (int i = 0; i < srv->argc; i++) argv[i] = xstrdup(srv->argv[i] ? srv->argv[i] : "");
    return argv;
}

static void argv_free(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) secure_free(argv[i]);
    free(argv);
}

static char *server_cwd(const struct tny_ctx *ctx, const mcp_imported_server *server) {
    if (!server || !server->cwd || !*server->cwd) return xstrdup(ctx->cwd);
    if (server->cwd[0] == '/') return xstrdup(server->cwd);
    return path_join(ctx->cwd, server->cwd);
}

/* ---- slot registry (g_mu held for every access) ---- */

static mcp_server *slot_find(const char *name) {
    for (int i = 0; i < g_nservers; i++) {
        mcp_server *s = &g_servers[i];
        if (s->name && !s->abandoned && strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

static mcp_server *slot_alloc(const char *name) {
    for (int i = 0; i < g_nservers; i++) {
        if (g_servers[i].state == SRV_EMPTY && !g_servers[i].name) {
            g_servers[i].name = xstrdup(name);
            return &g_servers[i];
        }
    }
    if (g_nservers >= MCP_MAX_SERVERS) return NULL;
    mcp_server *s = &g_servers[g_nservers++];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    return s;
}

/* ---- background warm-up ---- */

typedef struct {
    int slot;
    char **argv;
    char *cwd;
    mcp_env_pair env[MCP_IMPORT_MAX_ENV];
    int nenv;
} mcp_warm_job;

static void warm_job_free(mcp_warm_job *j) {
    argv_free(j->argv);
    free(j->cwd);
    for (int i = 0; i < j->nenv; i++) {
        free(j->env[i].key);
        secure_free(j->env[i].value);
    }
    free(j);
}

static void *warm_main(void *arg) {
    mcp_warm_job *j = arg;
    mcp_conn c;
    mcp_imported_server tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.nenv = j->nenv;
    for (int i = 0; i < j->nenv; i++) tmp.env[i] = j->env[i];
    int rc = conn_open(&c, j->argv, j->cwd, j->nenv ? &tmp : NULL);
    if (rc == 0) (void)conn_tools(&c); /* prefetch the catalog */
    pthread_mutex_lock(&g_mu);
    mcp_server *s = &g_servers[j->slot];
    if (s->abandoned) { /* shutdown gave up on us: last owner cleans up */
        conn_close(&c);
        free(s->name);
        memset(s, 0, sizeof *s);
    } else {
        s->conn = c; /* zeroed by conn_close inside conn_open on failure */
        s->state = rc == 0 ? SRV_READY : SRV_FAILED;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    warm_job_free(j);
    return NULL;
}

void mcp_warm_start(struct tny_ctx *ctx) {
    if (!ctx || ctx->mcp_disabled || ctx->library_mode) return;
    pthread_mutex_lock(&g_mu);
    bool started = g_warm_started;
    g_warm_started = true;
    pthread_mutex_unlock(&g_mu);
    if (started) return;
    mcp_catalog *cat = mcp_catalog_load(ctx);
    if (!cat) return;
    for (int i = 0; i < cat->nservers; i++) {
        const mcp_imported_server *srv = &cat->servers[i];
        if (!srv->name || srv->skipped) continue;
        char **argv = argv_from_server(srv);
        if (!argv) continue;
        pthread_mutex_lock(&g_mu);
        mcp_server *s = slot_find(srv->name) ? NULL : slot_alloc(srv->name);
        if (!s) {
            pthread_mutex_unlock(&g_mu);
            argv_free(argv);
            continue;
        }
        s->state = SRV_WARMING;
        int slot = (int)(s - g_servers);
        pthread_mutex_unlock(&g_mu);

        mcp_warm_job *j = calloc(1, sizeof *j);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int rc = -1;
        if (j) {
            j->slot = slot;
            j->argv = argv;
            j->cwd = server_cwd(ctx, srv);
            j->nenv = srv->nenv < MCP_IMPORT_MAX_ENV ? srv->nenv : MCP_IMPORT_MAX_ENV;
            for (int e = 0; e < j->nenv; e++) {
                j->env[e].key = xstrdup(srv->env[e].key);
                j->env[e].value = xstrdup(srv->env[e].value);
            }
            rc = pthread_create(&th, &at, warm_main, j);
        }
        pthread_attr_destroy(&at);
        if (rc != 0) { /* no thread (wasm): stay lazy, today's behavior */
            pthread_mutex_lock(&g_mu);
            free(s->name);
            memset(s, 0, sizeof *s);
            pthread_mutex_unlock(&g_mu);
            if (j) warm_job_free(j); /* frees argv too */
            else argv_free(argv);
        }
    }
    mcp_catalog_free(cat);
}

static mcp_catalog *load_profile(tools_env *env) {
    /* server mode (`tny acp`): the ACP client owns MCP — never read the
     * local profile (docs/backends/acp.md) */
    if (!env || !env->ctx || env->ctx->mcp_disabled) return NULL;
    return mcp_catalog_load(env->ctx);
}

/* Server ready for a call on this thread. Waits out a warm-up in flight
 * (the prewarm-take contract); a failed or never-warmed server is started
 * synchronously here, so the lazy path and its errors are unchanged. */
static mcp_server *get_server(tools_env *env, const char *name, char **err) {
    *err = NULL;
    pthread_mutex_lock(&g_mu);
    mcp_server *s = slot_find(name);
    while (s && s->state == SRV_WARMING) {
        pthread_cond_wait(&g_cv, &g_mu);
        s = slot_find(name);
    }
    if (s && s->state == SRV_READY) {
        pthread_mutex_unlock(&g_mu);
        return s;
    }
    pthread_mutex_unlock(&g_mu);

    mcp_catalog *prof = load_profile(env);
    if (!prof || prof->nservers == 0) {
        mcp_catalog_free(prof);
        *err = env->ctx->n_mcp_import_sources ? tool_err("no MCP servers configured")
                                              : tool_err("no ~/.tny/mcp.json profile");
        return NULL;
    }
    const mcp_imported_server *conf = mcp_catalog_find(prof, name);
    char **argv = conf ? argv_from_server(conf) : NULL;
    mcp_imported_server env_copy;
    memset(&env_copy, 0, sizeof env_copy);
    bool skipped = conf && conf->skipped;
    char *cwd = conf ? server_cwd(env->ctx, conf) : NULL;
    if (conf) {
        env_copy.nenv = conf->nenv < MCP_IMPORT_MAX_ENV ? conf->nenv : MCP_IMPORT_MAX_ENV;
        for (int i = 0; i < env_copy.nenv; i++) {
            env_copy.env[i].key = xstrdup(conf->env[i].key);
            env_copy.env[i].value = xstrdup(conf->env[i].value);
        }
    }
    if (!conf) {
        *err = env->ctx->n_mcp_import_sources
                   ? tool_err("no MCP server named %s", name)
                   : tool_err("no MCP server named %s in ~/.tny/mcp.json", name);
        mcp_catalog_free(prof);
        argv_free(argv);
        free(cwd);
        return NULL;
    }
    if (skipped) {
        *err = tool_err("could not start MCP server %s", name);
        mcp_catalog_free(prof);
        argv_free(argv);
        free(cwd);
        for (int i = 0; i < env_copy.nenv; i++) {
            free(env_copy.env[i].key);
            secure_free(env_copy.env[i].value);
        }
        return NULL;
    }
    mcp_catalog_free(prof);
    if (!argv) {
        *err = tool_err("could not start MCP server %s", name);
        free(cwd);
        for (int i = 0; i < env_copy.nenv; i++) {
            free(env_copy.env[i].key);
            secure_free(env_copy.env[i].value);
        }
        return NULL;
    }
    mcp_conn c;
    int rc = conn_open(&c, argv, cwd ? cwd : env->ctx->cwd, env_copy.nenv ? &env_copy : NULL);
    argv_free(argv);
    free(cwd);
    for (int i = 0; i < env_copy.nenv; i++) {
        free(env_copy.env[i].key);
        secure_free(env_copy.env[i].value);
    }

    pthread_mutex_lock(&g_mu);
    s = slot_find(name);
    if (!s) s = slot_alloc(name);
    if (!s) {
        pthread_mutex_unlock(&g_mu);
        if (rc == 0) conn_close(&c);
        *err = tool_err("could not start MCP server %s", name);
        return NULL;
    }
    if (rc == 0) {
        conn_close(&s->conn); /* drop a stale failed conn, if any */
        s->conn = c;
        s->state = SRV_READY;
    } else {
        s->state = SRV_FAILED;
    }
    pthread_mutex_unlock(&g_mu);
    if (rc != 0) {
        *err = tool_err("could not start MCP server %s", name);
        return NULL;
    }
    return s;
}

/* ---- catalog rendering ---- */

/* "server/tool — first line of description", capped, UTF-8 safe. */
static void append_tool_line(buf_t *out, const char *srv, const char *tool, const char *desc) {
    buf_appendf(out, "%s/%s — ", srv, tool);
    if (desc && *desc) {
        size_t n = 0;
        while (desc[n] && desc[n] != '\n' && n < MCP_CATALOG_DESC_MAX) n++;
        while (n && (desc[n] & 0xC0) == 0x80) n--; /* no split code point */
        buf_append(out, desc, n);
        if (desc[n]) buf_appends(out, "…");
    }
    buf_appends(out, "\n");
}

/* tools array of a cached tools/list doc, or NULL. */
static yyjson_val *tools_of(yyjson_doc *tl) {
    if (!tl) return NULL;
    yyjson_val *tools = jget(jget(yyjson_doc_get_root(tl), "result"), "tools");
    return tools && yyjson_is_arr(tools) ? tools : NULL;
}

void mcp_catalog_collect(struct tny_ctx *ctx, buf_t *out) {
    if (!ctx || ctx->mcp_disabled || ctx->library_mode) return;
    buf_t body;
    buf_init(&body);
    int listed = 0, extra = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nservers; i++) {
        mcp_server *s = &g_servers[i];
        if (!s->name || s->abandoned || s->state != SRV_READY) continue;
        yyjson_val *tools = tools_of(s->conn.tools);
        if (!tools) continue;
        size_t idx, max;
        yyjson_val *t;
        yyjson_arr_foreach(tools, idx, max, t) {
            const char *tn = jget_str(t, "name");
            if (!tn) continue;
            if (listed >= MCP_CATALOG_MAX_TOOLS) {
                extra++;
                continue;
            }
            buf_appends(&body, "- ");
            append_tool_line(&body, s->name, tn, jget_str(t, "description"));
            listed++;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (listed > 0) {
        buf_appends(out, "\nMCP tools (call with mcp_select_tool(server, tool, arguments)):\n");
        buf_append(out, body.data, body.len);
        if (extra > 0)
            buf_appendf(out, "(+%d more MCP tools — find them with mcp_search_tools)\n", extra);
    }
    buf_free(&body);
}

static const char *live_label(const char *name, bool skipped) {
    if (skipped) return "skipped";
    pthread_mutex_lock(&g_mu);
    mcp_server *s = slot_find(name);
    mcp_state st = s ? s->state : SRV_EMPTY;
    pthread_mutex_unlock(&g_mu);
    return st == SRV_READY     ? "connected"
           : st == SRV_WARMING ? "starting"
           : st == SRV_FAILED  ? "failed to start (a call retries)"
                               : "configured, not started";
}

char *mcp_features(tools_env *env) {
    mcp_catalog *prof = load_profile(env);
    if (!prof || prof->nservers == 0) {
        mcp_catalog_free(prof);
        return xstrdup("no MCP servers configured (create ~/.tny/mcp.json with "
                       "{\"servers\":{\"name\":{\"command\":[\"…\"]}}})");
    }
    buf_t out;
    buf_init(&out);
    for (int i = 0; i < prof->nservers; i++) {
        const mcp_imported_server *srv = &prof->servers[i];
        if (!srv->name) continue;
        if (srv->source == MCP_SRC_TNY)
            buf_appendf(&out, "%s (%s)\n", srv->name, live_label(srv->name, srv->skipped));
        else
            buf_appendf(&out, "%s (%s) [%s]\n", srv->name, live_label(srv->name, srv->skipped),
                        srv->origin ? srv->origin : "tny");
    }
    if (!out.len) buf_appends(&out, "(no MCP servers configured)");
    mcp_catalog_free(prof);
    return buf_detach(&out);
}

/* AND-match every whitespace token of query_lc against the lowercased
 * "name description" haystack. An empty query matches everything. */
static bool tool_matches(const char *name, const char *desc, const char *query_lc) {
    buf_t hay;
    buf_init(&hay);
    buf_appendf(&hay, "%s %s", name, desc ? desc : "");
    for (size_t i = 0; i < hay.len; i++) hay.data[i] = (char)tolower((unsigned char)hay.data[i]);
    bool ok = !buf_oom(&hay);
    const char *p = query_lc;
    while (ok && *p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        char *tok = xstrndup(p, (size_t)(e - p));
        if (!strstr(hay.data, tok)) ok = false;
        free(tok);
        p = e;
    }
    buf_free(&hay);
    return ok;
}

static bool query_is_empty(const char *q) {
    if (!q) return true;
    while (*q) {
        if (!isspace((unsigned char)*q)) return false;
        q++;
    }
    return true;
}

char *mcp_search_tools(tools_env *env, const char *query) {
    if (env->ctx->mcp_disabled) return xstrdup("no MCP servers configured");
    buf_t out;
    buf_init(&out);

    if (query_is_empty(query)) {
        /* empty query: dump the cached catalog, start or wait for nothing */
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_nservers; i++) {
            mcp_server *s = &g_servers[i];
            if (!s->name || s->abandoned || s->state != SRV_READY) continue;
            yyjson_val *tools = tools_of(s->conn.tools);
            if (!tools) continue;
            size_t idx, max;
            yyjson_val *t;
            yyjson_arr_foreach(tools, idx, max, t) {
                const char *tn = jget_str(t, "name");
                if (tn) append_tool_line(&out, s->name, tn, jget_str(t, "description"));
            }
        }
        pthread_mutex_unlock(&g_mu);
        if (!out.len) buf_appends(&out, "(no MCP tools cached yet — search with keywords)");
        buf_appends(&out, "\nCall one with mcp_select_tool(server, tool, arguments).");
        return buf_detach(&out);
    }

    char *query_lc = xstrdup(query);
    for (char *p = query_lc; *p; p++) *p = (char)tolower((unsigned char)*p);

    mcp_catalog *prof = load_profile(env);
    if (!prof) {
        free(query_lc);
        buf_free(&out);
        return xstrdup("no MCP servers configured");
    }
    for (int i = 0; i < prof->nservers; i++) {
        const mcp_imported_server *srv = &prof->servers[i];
        if (!srv->name || srv->skipped) continue;
        char *err = NULL;
        mcp_server *s = get_server(env, srv->name, &err);
        free(err); /* a server that will not start is silent in search */
        if (!s) continue;
        yyjson_val *tools = tools_of(conn_tools(&s->conn));
        if (!tools) continue;
        size_t i2, m2;
        yyjson_val *t;
        yyjson_arr_foreach(tools, i2, m2, t) {
            const char *tn = jget_str(t, "name");
            const char *td = jget_str(t, "description");
            if (!tn) continue;
            if (tool_matches(tn, td, query_lc)) append_tool_line(&out, srv->name, tn, td);
        }
    }
    mcp_catalog_free(prof);
    free(query_lc);
    if (!out.len) buf_appends(&out, "(no matching MCP tools)");
    buf_appends(&out, "\nCall one with mcp_select_tool(server, tool, arguments).");
    return buf_detach(&out);
}

char *mcp_call_tool(tools_env *env, const char *server, const char *tool, const char *args_json) {
    if (!server || !tool) return tool_err("mcp_select_tool needs server and tool");
    char *err = NULL;
    mcp_server *s = get_server(env, server, &err);
    if (!s) return err;
    buf_t params;
    buf_init(&params);
    buf_appends(&params, "{\"name\":");
    jescape(&params, tool);
    buf_appendf(&params, ",\"arguments\":%s}", args_json ? args_json : "{}");
    yyjson_doc *resp = rpc(&s->conn, "tools/call", params.data);
    buf_free(&params);
    if (!resp) return tool_err("MCP call to %s/%s timed out", server, tool);
    yyjson_val *root = yyjson_doc_get_root(resp);
    yyjson_val *jerr = jget(root, "error");
    buf_t out;
    buf_init(&out);
    if (jerr) {
        buf_appendf(&out, "error: MCP %s/%s: %s", server, tool,
                    jget_str(jerr, "message") ? jget_str(jerr, "message") : "unknown");
    } else {
        yyjson_val *content = jget(jget(root, "result"), "content");
        if (content && yyjson_is_arr(content)) {
            size_t idx, max;
            yyjson_val *c;
            yyjson_arr_foreach(content, idx, max, c) {
                const char *text = jget_str(c, "text");
                if (text) {
                    buf_appends(&out, text);
                    buf_appends(&out, "\n");
                }
            }
        }
        if (!out.len) {
            char *raw = jwrite_val(jget(root, "result"));
            if (raw) {
                buf_appends(&out, raw);
                free(raw);
            }
        }
    }
    yyjson_doc_free(resp);
    char *bounded = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return bounded;
}

char *mcp_list_text(struct tny_ctx *ctx) {
    mcp_catalog *cat = mcp_catalog_load(ctx);
    buf_t out;
    buf_init(&out);
    if (!cat || cat->nservers == 0) {
        buf_appends(&out, "no MCP servers configured (create ~/.tny/mcp.json, or set "
                          "mcp.import_from in ~/.tny/settings.json)\n");
    } else {
        for (int i = 0; i < cat->nservers; i++) {
            const mcp_imported_server *s = &cat->servers[i];
            const char *name = s->name ? s->name : "?";
            buf_appendf(&out, "%-20s %-12s %-7s %-7s %s", name, s->origin ? s->origin : "tny",
                        s->scope ? s->scope : "user", s->transport ? s->transport : "stdio",
                        live_label(name, s->skipped));
            if (s->skipped && s->skip_reason) buf_appendf(&out, " (%s)", s->skip_reason);
            buf_appends(&out, "\n");
        }
    }
    if (cat) {
        for (int i = 0; i < cat->nnotices; i++) {
            if (cat->notices[i]) buf_appendf(&out, "note: %s\n", cat->notices[i]);
        }
    }
    mcp_catalog_free(cat);
    return buf_detach(&out);
}

char *mcp_list_json(struct tny_ctx *ctx) {
    mcp_catalog *cat = mcp_catalog_load(ctx);
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"kind\":\"mcp_servers\",\"servers\":[");
    if (cat) {
        for (int i = 0; i < cat->nservers; i++) {
            const mcp_imported_server *s = &cat->servers[i];
            const char *name = s->name ? s->name : "";
            if (i) buf_appends(&out, ",");
            buf_appends(&out, "{\"name\":");
            jescape(&out, name);
            buf_appends(&out, ",\"source\":");
            jescape(&out, s->origin ? s->origin : "tny");
            buf_appends(&out, ",\"scope\":");
            jescape(&out, s->scope ? s->scope : "user");
            buf_appends(&out, ",\"transport\":");
            jescape(&out, s->transport ? s->transport : "stdio");
            buf_appends(&out, ",\"status\":");
            jescape(&out, live_label(name, s->skipped));
            buf_appendf(&out, ",\"skipped\":%s", s->skipped ? "true" : "false");
            if (s->skip_reason) {
                buf_appends(&out, ",\"skip_reason\":");
                jescape(&out, s->skip_reason);
            }
            buf_appends(&out, "}");
        }
    }
    buf_appends(&out, "],\"notices\":[");
    if (cat) {
        for (int i = 0; i < cat->nnotices; i++) {
            if (i) buf_appends(&out, ",");
            jescape(&out, cat->notices[i] ? cat->notices[i] : "");
        }
    }
    buf_appends(&out, "]}\n");
    mcp_catalog_free(cat);
    return buf_detach(&out);
}
