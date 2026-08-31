/* test_mcp.c — MCP background warm-up, cached catalog, token search
 * (docs/adr/0049). Fake stdio JSONL servers under a throwaway $HOME so
 * nothing touches the real ~/.tny and no real MCP server is spawned. */
#include "greatest.h"
#include "core/config.h"
#include "core/tools.h"
#include "mcp/mcp.h"
#include "mcp/mcp_import.h"
#include "util/toml.h"
#include "util/util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>

static char m_home[512], m_ws[520], m_bin[sizeof m_home + sizeof "/fake-mcp.sh"];

/* Fake MCP server: answers initialize / tools/list / tools/call in JSONL,
 * matching ids by counting id-bearing requests (tny numbers them 1..n per
 * server). Notifications fall through unanswered. */
static const char *FAKE_SERVER =
    "#!/bin/sh\n"
    "[ -n \"$1\" ] && sleep \"$1\"\n"
    "n=0\n"
    "while IFS= read -r line; do\n"
    "  case \"$line\" in\n"
    "  *'\"method\":\"initialize\"'*) n=$((n+1));\n"
    "    printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":"
    "\"2025-06-18\"}}\\n' \"$n\" ;;\n"
    "  *'\"method\":\"tools/list\"'*) n=$((n+1));\n"
    "    printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":["
    "{\"name\":\"deploy_app\",\"description\":\"Deploy the application to production\"},"
    "{\"name\":\"rollback\",\"description\":\"Roll back the most recent deploy\"}]}}\\n' "
    "\"$n\" ;;\n"
    "  *'\"method\":\"tools/call\"'*) n=$((n+1));\n"
    "    printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":["
    "{\"type\":\"text\",\"text\":\"called ok\"}]}}\\n' \"$n\" ;;\n"
    "  esac\n"
    "done\n";

/* Reads stdin forever, answers nothing, exits on EOF: a server that hangs
 * in its handshake without outliving the test process. */
static const char *HUNG_SERVER = "#!/bin/sh\nwhile IFS= read -r line; do :; done\n";

static void mcp_test_env(void) {
    if (m_home[0]) return;
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(m_home, sizeof m_home, "%s/tny-mcp-home-XXXXXX", t);
    if (!mkdtemp(m_home)) abort();
    setenv("HOME", m_home, 1);
    unsetenv("CODEX_HOME");
    snprintf(m_ws, sizeof m_ws, "%s/ws", m_home);
    mkdir_p(m_ws);
    char tnydir[600];
    snprintf(tnydir, sizeof tnydir, "%s/.tny", m_home);
    mkdir_p(tnydir);
    char settings[620];
    snprintf(settings, sizeof settings, "%s/settings.json", tnydir);
    file_write_atomic(settings, "{}", 2);
    snprintf(m_bin, sizeof m_bin, "%s/fake-mcp.sh", m_home);
    file_write_atomic(m_bin, FAKE_SERVER, strlen(FAKE_SERVER));
    chmod(m_bin, 0755);
}

static void write_profile(const char *json) {
    char path[620];
    snprintf(path, sizeof path, "%s/.tny/mcp.json", m_home);
    if (json) file_write_atomic(path, json, strlen(json));
    else unlink(path);
}

static char *fast_profile(void) {
    static char json[700];
    snprintf(json, sizeof json, "{\"servers\":{\"srv\":{\"command\":[\"%s\"]}}}", m_bin);
    return json;
}

/* Poll the cached catalog until it names srv/deploy_app (warm finished). */
static bool catalog_ready(tny_ctx *ctx, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        buf_t cat;
        buf_init(&cat);
        mcp_catalog_collect(ctx, &cat);
        bool ok = cat.data && strstr(cat.data, "srv/deploy_app");
        buf_free(&cat);
        if (ok) return true;
        if (now_ms() >= deadline) return false;
        usleep(20 * 1000);
    }
}

/* Session start spawns the warm-up off the loop (a server that sleeps before
 * answering must not delay mcp_warm_start), and the catalog then appears in
 * the system-prompt collector without any search call. */
TEST warm_start_does_not_block_and_catalog_appears(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    char json[700];
    snprintf(json, sizeof json, "{\"servers\":{\"srv\":{\"command\":[\"%s\",\"0.4\"]}}}", m_bin);
    write_profile(json);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);

    int64_t t0 = now_ms();
    mcp_warm_start(ctx);
    ASSERT(now_ms() - t0 < 350); /* the 0.4s handshake runs on the thread */

    buf_t cat;
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat); /* mid-warm: empty, and non-blocking */
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);

    ASSERT(catalog_ready(ctx, 8000));
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT(strstr(cat.data, "srv/deploy_app — Deploy the application to production"));
    ASSERT(strstr(cat.data, "srv/rollback"));
    ASSERT(strstr(cat.data, "mcp_select_tool"));
    buf_free(&cat);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* A select against a server still warming waits for the handshake (the
 * prewarm-take contract) and then calls through the warmed connection. */
TEST select_waits_for_warming_server(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    char json[700];
    snprintf(json, sizeof json, "{\"servers\":{\"srv\":{\"command\":[\"%s\",\"0.3\"]}}}", m_bin);
    write_profile(json);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};

    mcp_warm_start(ctx);
    char *out = mcp_call_tool(&env, "srv", "deploy_app", "{}");
    ASSERT(out);
    ASSERT(strstr(out, "called ok"));
    free(out);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* A server that never answers its handshake must not stall anything except
 * a call that names it: warm start, catalog, features, empty-query search,
 * and shutdown all return immediately while it hangs. */
TEST hung_server_does_not_stall(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    char hung[560];
    snprintf(hung, sizeof hung, "%s/hung-mcp.sh", m_home);
    file_write_atomic(hung, HUNG_SERVER, strlen(HUNG_SERVER));
    chmod(hung, 0755);
    char json[1400];
    snprintf(json, sizeof json,
             "{\"servers\":{\"slow\":{\"command\":[\"%s\"]},\"srv\":{\"command\":[\"%s\"]}}}", hung,
             m_bin);
    write_profile(json);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};

    int64_t t0 = now_ms();
    mcp_warm_start(ctx);
    ASSERT(now_ms() - t0 < 350);

    ASSERT(catalog_ready(ctx, 8000)); /* srv arrives while slow hangs */
    t0 = now_ms();
    char *feats = mcp_features(&env);
    char *empty = mcp_search_tools(&env, "");
    ASSERT(now_ms() - t0 < 1000);
    ASSERT(strstr(feats, "slow (starting)"));
    ASSERT(strstr(feats, "srv (connected)"));
    ASSERT(strstr(empty, "srv/deploy_app")); /* cached list, no waiting */
    ASSERT(!strstr(empty, "slow/"));
    free(feats);
    free(empty);

    t0 = now_ms();
    mcp_shutdown_all(); /* abandons the warming slot instead of waiting */
    ASSERT(now_ms() - t0 < 500);
    tny_ctx_free(ctx);
    PASS();
}

/* Search is a token AND-match over name + description, not one contiguous
 * substring; an empty query lists the cached catalog. */
TEST search_matches_tokens_not_substring(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    write_profile(fast_profile());
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    ASSERT(catalog_ready(ctx, 8000));

    /* "deploy production" is not a substring of either description */
    char *out = mcp_search_tools(&env, "deploy production");
    ASSERT(strstr(out, "srv/deploy_app"));
    ASSERT(!strstr(out, "srv/rollback")); /* has "deploy", lacks "production" */
    free(out);

    out = mcp_search_tools(&env, "Recent ROLL");
    ASSERT(strstr(out, "srv/rollback"));
    ASSERT(!strstr(out, "srv/deploy_app"));
    free(out);

    out = mcp_search_tools(&env, "zzz-no-such-tool");
    ASSERT(strstr(out, "no matching MCP tools"));
    free(out);

    out = mcp_search_tools(&env, NULL); /* empty query: the cached catalog */
    ASSERT(strstr(out, "srv/deploy_app"));
    ASSERT(strstr(out, "srv/rollback"));
    free(out);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* A server whose command cannot start is silent everywhere until a call
 * names it; the call then reports the existing spawn error. */
TEST failed_server_silent_until_call(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    write_profile("{\"servers\":{\"broken\":{\"command\":[\"/nonexistent-tny-mcp\"]}}}");
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);

    buf_t cat;
    buf_init(&cat);
    int64_t deadline = now_ms() + 5000;
    char *feats = NULL;
    for (;;) { /* wait out the warm attempt so the state is FAILED, not WARMING */
        feats = mcp_features(&env);
        if (strstr(feats, "broken (failed to start")) break;
        free(feats);
        feats = NULL;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    free(feats);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d"); /* failure is invisible in context */
    buf_free(&cat);

    char *out = mcp_call_tool(&env, "broken", "x", "{}");
    ASSERT(strstr(out, "error: could not start MCP server broken"));
    free(out);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* Workspace mcp.json files never load (cloning a repo must not start a
 * server), and `tny acp` server mode (mcp_disabled) never warms even with
 * a valid user profile. */
TEST workspace_profile_never_loads_and_acp_never_warms(void) {
    mcp_test_env();
    mcp_shutdown_all();  /* clear a warm latch an earlier suite may have set */
    write_profile(NULL); /* no ~/.tny/mcp.json */
    char wsjson[620];
    snprintf(wsjson, sizeof wsjson, "%s/mcp.json", m_ws);
    file_write_atomic(wsjson, fast_profile(), strlen(fast_profile()));
    char wstny[640];
    snprintf(wstny, sizeof wstny, "%s/.tny", m_ws);
    mkdir_p(wstny);
    snprintf(wstny, sizeof wstny, "%s/.tny/mcp.json", m_ws);
    file_write_atomic(wstny, fast_profile(), strlen(fast_profile()));

    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    usleep(100 * 1000);
    buf_t cat;
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);
    char *feats = mcp_features(&env);
    ASSERT(strstr(feats, "no MCP servers configured"));
    free(feats);
    mcp_shutdown_all();

    /* disabled runtime: a valid profile still must not warm or list */
    write_profile(fast_profile());
    ctx->mcp_disabled = true;
    mcp_warm_start(ctx);
    usleep(100 * 1000);
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);
    char *out = mcp_search_tools(&env, "deploy");
    ASSERT(strstr(out, "no MCP servers configured"));
    free(out);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* ---- Streamable HTTP (docs/adr/0051) ---- */

typedef struct {
    int listen_fd;
    pthread_t thread;
    int mode; /* 0 modern json, 1 modern sse, 2 legacy initialize, 3 401 */
    int ready;
    int saw_get;
    int saw_literal_auth;
    char last_method[64];
    char last_mcp_method[64];
    char last_mcp_name[64];
    char last_version[32];
    char last_auth[128];
    char last_workspace[64];
    char last_session[64];
    int discover_count;
    int initialize_count;
    int initialized_count;
} http_mock;

static int mock_listen(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int mock_port(int fd) {
    struct sockaddr_in sa = {0};
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) return -1;
    return (int)ntohs(sa.sin_port);
}

static char *find_bytes(char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    return NULL;
}

static const char *header_ci(const char *req, const char *name) {
    size_t nlen = strlen(name);
    for (const char *p = req; *p; p++) {
        if (p != req && p[-1] != '\n') continue;
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') return p + nlen + 1;
    }
    return NULL;
}

static int read_http_request(int cfd, buf_t *req) {
    for (;;) {
        char tmp[4096];
        ssize_t n = read(cfd, tmp, sizeof tmp);
        if (n <= 0) return -1;
        buf_append(req, tmp, (size_t)n);
        if (req->len >= 4) {
            char *end = find_bytes(req->data, req->len, "\r\n\r\n", 4);
            if (end) {
                size_t hdr = (size_t)(end - req->data) + 4;
                const char *cl = header_ci(req->data, "Content-Length");
                size_t body = 0;
                if (cl && cl < end) body = (size_t)strtoul(cl, NULL, 10);
                if (req->len >= hdr + body) return 0;
            }
        }
    }
}

static void write_all_fd(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += (size_t)w;
        n -= (size_t)w;
    }
}

static void rpc_result(const char *body, const char *method, char *out, size_t cap) {
    const char *id = "1";
    const char *idp = strstr(body, "\"id\":");
    static char idbuf[16];
    if (idp) {
        idp += 5;
        size_t n = 0;
        while (idp[n] && idp[n] != ',' && idp[n] != '}' && n + 1 < sizeof idbuf) {
            idbuf[n] = idp[n];
            n++;
        }
        idbuf[n] = 0;
        id = idbuf;
    }
    const char *result = "{}";
    if (strstr(method, "server/discover") || strstr(body, "\"method\":\"server/discover\""))
        result = "{\"supportedVersions\":[\"2026-07-28\"],\"capabilities\":{\"tools\":{}}}";
    else if (strstr(method, "tools/list") || strstr(body, "\"method\":\"tools/list\""))
        result = "{\"tools\":[{\"name\":\"deploy_app\",\"description\":\"Deploy the application "
                 "to production\"}]}";
    else if (strstr(method, "tools/call") || strstr(body, "\"method\":\"tools/call\""))
        result = "{\"content\":[{\"type\":\"text\",\"text\":\"called ok\"}]}";
    else if (strstr(method, "initialize") || strstr(body, "\"method\":\"initialize\""))
        result = "{\"protocolVersion\":\"2025-06-18\"}";
    snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
}

static void capture_header(const char *req, const char *name, char *out, size_t cap) {
    const char *p = header_ci(req, name);
    if (!p) {
        out[0] = 0;
        return;
    }
    while (*p == ' ') p++;
    size_t n = 0;
    while (p[n] && p[n] != '\r' && n + 1 < cap) n++;
    memcpy(out, p, n);
    out[n] = 0;
}

static void *http_mock_main(void *arg) {
    http_mock *m = arg;
    m->ready = 1;
    for (;;) {
        int cfd = accept(m->listen_fd, NULL, NULL);
        if (cfd < 0) break;
        buf_t req;
        buf_init(&req);
        if (read_http_request(cfd, &req) != 0) {
            buf_free(&req);
            close(cfd);
            continue;
        }
        if (!strncmp(req.data, "GET ", 4)) m->saw_get++;
        capture_header(req.data, "Mcp-Method", m->last_mcp_method, sizeof m->last_mcp_method);
        capture_header(req.data, "Mcp-Name", m->last_mcp_name, sizeof m->last_mcp_name);
        capture_header(req.data, "MCP-Protocol-Version", m->last_version, sizeof m->last_version);
        capture_header(req.data, "Authorization", m->last_auth, sizeof m->last_auth);
        capture_header(req.data, "X-Workspace", m->last_workspace, sizeof m->last_workspace);
        capture_header(req.data, "Mcp-Session-Id", m->last_session, sizeof m->last_session);
        if (strstr(req.data, "\"method\":\"server/discover\"")) m->discover_count++;
        if (strstr(req.data, "\"method\":\"initialize\"")) m->initialize_count++;
        if (strstr(req.data, "\"method\":\"notifications/initialized\"")) m->initialized_count++;
        if (strstr(req.data, "\r\nAuthorization: secret-literal")) m->saw_literal_auth = 1;
        const char *sp = strchr(req.data, ' ');
        snprintf(m->last_method, sizeof m->last_method, "%.*s",
                 sp && sp - req.data < 8 ? (int)(sp - req.data) : 4, req.data);

        if (m->mode == 3) {
            write_all_fd(
                cfd, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            buf_free(&req);
            close(cfd);
            continue;
        }
        if (m->mode == 2 && strstr(req.data, "\"method\":\"server/discover\"")) {
            write_all_fd(
                cfd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            buf_free(&req);
            close(cfd);
            continue;
        }
        char payload[768];
        rpc_result(req.data, m->last_mcp_method, payload, sizeof payload);
        char hdr[256];
        if (m->mode == 1 && strstr(req.data, "\"method\":\"tools/call\"")) {
            char sse[1024];
            snprintf(sse, sizeof sse,
                     "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                     "progress\"}\n\ndata: %s\n\n",
                     payload);
            snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     strlen(sse));
            write_all_fd(cfd, hdr);
            write_all_fd(cfd, sse);
        } else {
            const char *session = "";
            if (m->mode == 2 && strstr(req.data, "\"method\":\"initialize\""))
                session = "Mcp-Session-Id: sess-1\r\n";
            snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                     "%sConnection: close\r\n\r\n",
                     strlen(payload), session);
            write_all_fd(cfd, hdr);
            write_all_fd(cfd, payload);
        }
        buf_free(&req);
        close(cfd);
    }
    return NULL;
}

static http_mock *start_http_mock(int mode) {
    http_mock *m = calloc(1, sizeof *m);
    m->mode = mode;
    m->listen_fd = mock_listen();
    if (m->listen_fd < 0) abort();
    pthread_t th;
    pthread_create(&th, NULL, http_mock_main, m);
    m->thread = th;
    while (!m->ready) usleep(1000);
    return m;
}

static void stop_http_mock(http_mock *m) {
    shutdown(m->listen_fd, SHUT_RDWR);
    close(m->listen_fd);
    pthread_join(m->thread, NULL);
    free(m);
}

static void write_http_profile(http_mock *m, const char *extra) {
    char json[900];
    snprintf(json, sizeof json,
             "{\"servers\":{\"remote\":{\"type\":\"http\",\"url\":\"http://127.0.0.1:%d/mcp\"%s}}}",
             mock_port(m->listen_fd), extra ? extra : "");
    write_profile(json);
}

TEST http_modern_json_warms_and_calls(void) {
    mcp_test_env();
    mcp_shutdown_all();
    http_mock *m = start_http_mock(0);
    write_http_profile(m, NULL);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    int64_t deadline = now_ms() + 8000;
    buf_t cat;
    for (;;) {
        buf_init(&cat);
        mcp_catalog_collect(ctx, &cat);
        bool ok = cat.data && strstr(cat.data, "remote/deploy_app");
        buf_free(&cat);
        if (ok) break;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    ASSERT(m->discover_count >= 1);
    ASSERT_EQ(0, m->initialize_count);
    ASSERT_STR_EQ("tools/list", m->last_mcp_method);
    char *out = mcp_call_tool(&env, "remote", "deploy_app", "{}");
    ASSERT(out);
    ASSERT(strstr(out, "called ok"));
    free(out);
    ASSERT_STR_EQ("tools/call", m->last_mcp_method);
    ASSERT_STR_EQ("deploy_app", m->last_mcp_name);
    ASSERT_STR_EQ("2026-07-28", m->last_version);
    ASSERT_EQ(0, m->saw_get);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    stop_http_mock(m);
    PASS();
}

TEST http_sse_tools_call_fails_actionably(void) {
    mcp_test_env();
    mcp_shutdown_all();
    http_mock *m = start_http_mock(1);
    write_http_profile(m, NULL);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    int64_t deadline = now_ms() + 8000;
    for (;;) {
        buf_t cat;
        buf_init(&cat);
        mcp_catalog_collect(ctx, &cat);
        bool ok = cat.data && strstr(cat.data, "remote/deploy_app");
        buf_free(&cat);
        if (ok) break;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    char *out = mcp_call_tool(&env, "remote", "deploy_app", "{}");
    ASSERT(out);
    ASSERT(strstr(out, "unsupported SSE response transport"));
    ASSERT(strstr(out, "Streamable HTTP POST"));
    ASSERT(!strstr(out, "called ok"));
    free(out);
    ASSERT_EQ(0, m->saw_get);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    stop_http_mock(m);
    PASS();
}

TEST http_legacy_initialize_fallback(void) {
    mcp_test_env();
    mcp_shutdown_all();
    http_mock *m = start_http_mock(2);
    write_http_profile(m, NULL);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    int64_t deadline = now_ms() + 8000;
    for (;;) {
        buf_t cat;
        buf_init(&cat);
        mcp_catalog_collect(ctx, &cat);
        bool ok = cat.data && strstr(cat.data, "remote/deploy_app");
        buf_free(&cat);
        if (ok) break;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    char *out = mcp_call_tool(&env, "remote", "deploy_app", "{}");
    ASSERT(strstr(out, "called ok"));
    free(out);
    ASSERT(m->discover_count >= 1);
    ASSERT(m->initialize_count >= 1);
    ASSERT(m->initialized_count >= 1);
    ASSERT_STR_EQ("sess-1", m->last_session);
    ASSERT_EQ(0, m->saw_get);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    stop_http_mock(m);
    PASS();
}

TEST http_auth_from_env_and_rejects_literal_authorization(void) {
    mcp_test_env();
    mcp_shutdown_all();
    http_mock *m = start_http_mock(0);
    setenv("TNY_MCP_TOKEN", "tok-from-env", 1);
    setenv("TNY_MCP_WS", "acme", 1);
    write_http_profile(m, ",\"header_env\":{\"X-Workspace\":\"TNY_MCP_WS\"},"
                          "\"bearer_token_env\":\"TNY_MCP_TOKEN\"");
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    int64_t deadline = now_ms() + 8000;
    for (;;) {
        buf_t cat;
        buf_init(&cat);
        mcp_catalog_collect(ctx, &cat);
        bool ok = cat.data && strstr(cat.data, "remote/deploy_app");
        buf_free(&cat);
        if (ok) break;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    ASSERT(strstr(m->last_auth, "Bearer tok-from-env"));
    ASSERT_STR_EQ("acme", m->last_workspace);
    mcp_shutdown_all();
    tny_ctx_free(ctx);

    write_profile("{\"servers\":{\"bad\":{\"type\":\"http\",\"url\":\"http://127.0.0.1:9/mcp\","
                  "\"headers\":{\"Authorization\":\"secret-literal\"}}}}");
    ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    env.ctx = ctx;
    char *out = mcp_call_tool(&env, "bad", "x", "{}");
    ASSERT(strstr(out, "literal Authorization"));
    free(out);
    ASSERT_EQ(0, m->saw_literal_auth);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    stop_http_mock(m);
    PASS();
}

TEST http_401_is_silent_until_named(void) {
    mcp_test_env();
    mcp_shutdown_all();
    http_mock *m = start_http_mock(3);
    write_http_profile(m, NULL);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    int64_t deadline = now_ms() + 5000;
    char *feats = NULL;
    for (;;) {
        feats = mcp_features(&env);
        if (strstr(feats, "remote (failed to start")) break;
        free(feats);
        feats = NULL;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    free(feats);
    buf_t cat;
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);
    char *out = mcp_call_tool(&env, "remote", "x", "{}");
    ASSERT(strstr(out, "HTTP 401"));
    ASSERT(!strstr(out, "Authorization"));
    free(out);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    stop_http_mock(m);
    PASS();
}

static void write_settings(const char *json) {
    char path[620];
    snprintf(path, sizeof path, "%s/.tny/settings.json", m_home);
    file_write_atomic(path, json, strlen(json));
}

static void write_home(const char *rel, const char *body) {
    char path[720];
    snprintf(path, sizeof path, "%s/%s", m_home, rel);
    char dir[720];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir_p(dir);
    }
    file_write_atomic(path, body, strlen(body));
}

static bool copy_fixture_home(const char *fixture, const char *rel) {
    char path[720];
    snprintf(path, sizeof path, "tests/fixtures/mcp-import/%s", fixture);
    size_t len = 0;
    char *body = file_slurp(path, &len);
    if (!body) return false;
    write_home(rel, body);
    free(body);
    return true;
}

static bool copy_fixture_workspace(const char *fixture, const char *rel) {
    char source[720], path[720], dir[720];
    snprintf(source, sizeof source, "tests/fixtures/mcp-import/%s", fixture);
    size_t len = 0;
    char *body = file_slurp(source, &len);
    if (!body) return false;
    snprintf(path, sizeof path, "%s/%s", m_ws, rel);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir_p(dir);
    }
    bool ok = file_write_atomic(path, body, len) == 0;
    free(body);
    return ok;
}

/* Default-off: foreign files exist but are not opened until import_from
 * names the source. Native names win; remote entries are skipped. */
TEST import_off_by_default_then_merges_stdio(void) {
    mcp_test_env();
    mcp_shutdown_all();
    setenv("TNY_TEST_MCP_BIN", m_bin, 1);
    write_profile("{\"servers\":{\"shared\":{\"command\":[\"/bin/true\"]}}}");
    ASSERT(copy_fixture_home("codex.toml", ".codex/config.toml"));
    ASSERT(copy_fixture_home("claude-user.json", ".claude.json"));
    ASSERT(copy_fixture_workspace("claude-project.json", ".mcp.json"));
    ASSERT(copy_fixture_home("grok.toml", ".grok/config.toml"));
    ASSERT(copy_fixture_workspace("grok-project.toml", ".grok/config.toml"));
    ASSERT(copy_fixture_home("cursor-user.json", ".cursor/mcp.json"));
    ASSERT(copy_fixture_workspace("cursor-project.json", ".cursor/mcp.json"));

    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    mcp_catalog *cat = mcp_catalog_load(ctx);
    ASSERT(cat);
    ASSERT_EQ(1, cat->nservers);
    ASSERT(mcp_catalog_find(cat, "shared"));
    ASSERT(!mcp_catalog_find(cat, "cursor_user"));
    ASSERT(!mcp_catalog_find(cat, "codex_stdio"));
    mcp_catalog_free(cat);
    tny_ctx_free(ctx);

    write_settings("{\"mcp\":{\"import_from\":[\"codex\",\"claude\",\"grok\",\"cursor-agent\"]}}");
    ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    cat = mcp_catalog_load(ctx);
    ASSERT(cat);
    ASSERT_EQ(MCP_SRC_TNY, mcp_catalog_find(cat, "shared")->source);
    ASSERT_EQ(MCP_SRC_CURSOR, mcp_catalog_find(cat, "cursor_user")->source);
    ASSERT(mcp_catalog_find(cat, "cursor_project"));
    ASSERT(strcmp(mcp_catalog_find(cat, "cursor_project_wins")->scope, "project") == 0);
    ASSERT(mcp_catalog_find(cat, "claude_user"));
    ASSERT(mcp_catalog_find(cat, "claude_project"));
    ASSERT(strcmp(mcp_catalog_find(cat, "claude_project_wins")->scope, "project") == 0);
    ASSERT(mcp_catalog_find(cat, "grok_user"));
    ASSERT(mcp_catalog_find(cat, "grok_project"));
    ASSERT(strcmp(mcp_catalog_find(cat, "grok_project_wins")->scope, "project") == 0);
    const mcp_imported_server *codex = mcp_catalog_find(cat, "codex_stdio");
    ASSERT(codex);
    ASSERT(codex->argc >= 2);
    ASSERT(strcmp(codex->argv[0], "/bin/echo") == 0);
    ASSERT(codex->nenv == 1);
    ASSERT(strcmp(codex->env[0].key, "FIXTURE_TOKEN") == 0);
    const mcp_imported_server *remote = mcp_catalog_find(cat, "cursor_remote");
    ASSERT(remote);
    ASSERT(remote->skipped);
    ASSERT(strcmp(remote->transport, "http") == 0);
    const mcp_imported_server *sse = mcp_catalog_find(cat, "grok_sse");
    ASSERT(sse);
    ASSERT(sse->skipped);
    /* collision: native stays tny, cursor duplicate is a notice */
    bool saw_collision = false;
    for (int i = 0; i < cat->nnotices; i++)
        if (cat->notices[i] && strstr(cat->notices[i], "shared")) saw_collision = true;
    ASSERT(saw_collision);

    char *json = mcp_list_json(ctx);
    ASSERT(json);
    ASSERT(strstr(json, "\"kind\":\"mcp_servers\""));
    ASSERT(strstr(json, "\"source\":\"tny\""));
    ASSERT(strstr(json, "\"source\":\"cursor-agent\""));
    ASSERT(strstr(json, "\"source\":\"codex\""));
    ASSERT(strstr(json, "\"scope\":\"project\""));
    ASSERT(strstr(json, "\"transport\":\"http\""));
    ASSERT(!strstr(json, "fixture-secret"));
    ASSERT(!strstr(json, "FIXTURE_TOKEN"));
    free(json);

    tools_env env = {.ctx = ctx};
    char *called = mcp_call_tool(&env, "codex_runtime", "deploy_app", "{}");
    ASSERT(called);
    ASSERT(strstr(called, "called ok"));
    free(called);
    mcp_shutdown_all();
    mcp_catalog_free(cat);
    tny_ctx_free(ctx);
    PASS();
}

TEST import_malformed_source_is_notice_not_fatal(void) {
    mcp_test_env();
    mcp_shutdown_all();
    write_profile(NULL);
    write_settings("{\"mcp\":{\"import_from\":[\"cursor-agent\"]}}");
    ASSERT(copy_fixture_home("malformed.json", ".cursor/mcp.json"));
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    mcp_catalog *cat = mcp_catalog_load(ctx);
    ASSERT(cat);
    ASSERT_EQ(0, cat->nservers);
    bool saw = false;
    for (int i = 0; i < cat->nnotices; i++)
        if (cat->notices[i] && strstr(cat->notices[i], "malformed")) saw = true;
    ASSERT(saw);
    mcp_catalog_free(cat);
    tny_ctx_free(ctx);
    PASS();
}

TEST import_project_configs_are_opt_in_and_attributed(void) {
    mcp_test_env();
    mcp_shutdown_all();
    write_profile(NULL);
    write_settings("{\"mcp\":{\"import_from\":[\"cursor-agent\",\"claude\"]}}");
    char user_path[720];
    snprintf(user_path, sizeof user_path, "%s/.cursor/mcp.json", m_home);
    unlink(user_path);
    snprintf(user_path, sizeof user_path, "%s/.claude.json", m_home);
    unlink(user_path);
    char ws_cursor[640];
    snprintf(ws_cursor, sizeof ws_cursor, "%s/.cursor", m_ws);
    mkdir_p(ws_cursor);
    snprintf(ws_cursor, sizeof ws_cursor, "%s/.cursor/mcp.json", m_ws);
    const char *proj = "{\"mcpServers\":{\"proj\":{\"command\":\"npx\",\"args\":[\"x\"]}}}";
    file_write_atomic(ws_cursor, proj, strlen(proj));
    char ws_mcp[640];
    snprintf(ws_mcp, sizeof ws_mcp, "%s/.mcp.json", m_ws);
    const char *proj2 = "{\"mcpServers\":{\"proj2\":{\"command\":\"npx\",\"args\":[\"y\"]}}}";
    file_write_atomic(ws_mcp, proj2, strlen(proj2));
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    mcp_catalog *cat = mcp_catalog_load(ctx);
    ASSERT(cat);
    ASSERT(mcp_catalog_find(cat, "proj"));
    ASSERT(mcp_catalog_find(cat, "proj2"));
    ASSERT(strcmp(mcp_catalog_find(cat, "proj")->scope, "project") == 0);
    ASSERT(strcmp(mcp_catalog_find(cat, "proj2")->scope, "project") == 0);
    mcp_catalog_free(cat);
    tny_ctx_free(ctx);
    PASS();
}

TEST import_missing_and_malformed_toml_are_notices(void) {
    mcp_test_env();
    mcp_shutdown_all();
    write_profile(NULL);
    write_settings("{\"mcp\":{\"import_from\":[\"codex\",\"grok\"]}}");
    ASSERT(copy_fixture_home("malformed.toml", ".codex/config.toml"));
    char grok_user[720], grok_project[720];
    snprintf(grok_user, sizeof grok_user, "%s/.grok/config.toml", m_home);
    snprintf(grok_project, sizeof grok_project, "%s/.grok/config.toml", m_ws);
    unlink(grok_user);
    unlink(grok_project);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    mcp_catalog *cat = mcp_catalog_load(ctx);
    ASSERT(cat);
    bool malformed = false, missing = false;
    for (int i = 0; i < cat->nnotices; i++) {
        if (strstr(cat->notices[i], "codex") && strstr(cat->notices[i], "malformed"))
            malformed = true;
        if (strstr(cat->notices[i], "grok") && strstr(cat->notices[i], "no config")) missing = true;
    }
    ASSERT(malformed);
    ASSERT(missing);
    mcp_catalog_free(cat);
    tny_ctx_free(ctx);
    PASS();
}

TEST imported_stdio_server_runs_through_existing_permission_path(void) {
    mcp_test_env();
    mcp_shutdown_all();
    write_profile(NULL);
    write_settings("{\"mcp\":{\"import_from\":[\"cursor-agent\"]}}");
    char json[900];
    snprintf(json, sizeof json,
             "{\"mcpServers\":{\"imported\":{\"type\":\"stdio\",\"command\":\"%s\","
             "\"args\":[\"0\"],\"env\":{\"MCP_FIXTURE\":\"synthetic\"}}}}",
             m_bin);
    char dir[640], path[680];
    snprintf(dir, sizeof dir, "%s/.cursor", m_ws);
    mkdir_p(dir);
    snprintf(path, sizeof path, "%s/mcp.json", dir);
    ASSERT_EQ(0, file_write_atomic(path, json, strlen(json)));
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    char *out = mcp_call_tool(&env, "imported", "deploy_app", "{}");
    ASSERT(out);
    ASSERT(strstr(out, "called ok"));
    free(out);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

TEST toml_subset_parses_mcp_servers_tables(void) {
    const char *src = "# comment\n"
                      "model = \"gpt\"\n"
                      "[mcp_servers.ctx7]\n"
                      "command = \"npx\"\n"
                      "args = [\"-y\", \"@upstash/context7-mcp\"]\n"
                      "enabled = true\n"
                      "\n"
                      "[mcp_servers.ctx7.env]\n"
                      "API_KEY = \"x\"\n"
                      "\n"
                      "[[hooks.PreToolUse]]\n"
                      "matcher = \"^Bash$\"\n";
    yyjson_doc *doc = toml_parse_subset(src, strlen(src));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT(yyjson_is_obj(root));
    ASSERT(strcmp(jget_str(root, "model"), "gpt") == 0);
    yyjson_val *srv = jget(jget(root, "mcp_servers"), "ctx7");
    ASSERT(srv);
    ASSERT(strcmp(jget_str(srv, "command"), "npx") == 0);
    yyjson_val *args = jget(srv, "args");
    ASSERT(yyjson_is_arr(args));
    ASSERT_EQ(2, (int)yyjson_arr_size(args));
    ASSERT(jget_bool(srv, "enabled", false));
    ASSERT(strcmp(jget_str(jget(srv, "env"), "API_KEY"), "x") == 0);
    ASSERT(!jget(root, "hooks")); /* arrays of tables ignored */
    yyjson_doc_free(doc);
    PASS();
}

/* Hostile shapes: deep nesting must fail the parse (never smash the stack),
 * a server literally named `env` is a server table, and unsupported quoted
 * table headers are discarded instead of spilling keys onto the root. */
TEST toml_subset_survives_hostile_shapes(void) {
    char *deep = malloc(200000 + 32);
    ASSERT(deep);
    size_t n = (size_t)snprintf(deep, 32, "args = ");
    for (int i = 0; i < 100000; i++) deep[n++] = '[';
    deep[n] = '\0';
    yyjson_doc *doc = toml_parse_subset(deep, n);
    free(deep);
    if (doc) yyjson_doc_free(doc); /* either result is fine; crashing is not */

    const char *named_env = "[mcp_servers.env]\ncommand = \"x\"\nargs = [\"a\"]\n";
    doc = toml_parse_subset(named_env, strlen(named_env));
    ASSERT(doc);
    yyjson_val *srv = jget(jget(yyjson_doc_get_root(doc), "mcp_servers"), "env");
    ASSERT(srv);
    ASSERT(strcmp(jget_str(srv, "command"), "x") == 0);
    ASSERT(yyjson_is_arr(jget(srv, "args"))); /* args allowed: server table */
    yyjson_doc_free(doc);

    const char *quoted = "[mcp_servers.\"my.server\"]\ncommand = \"evil\"\n"
                         "[mcp_servers.ok]\ncommand = \"fine\"\n";
    doc = toml_parse_subset(quoted, strlen(quoted));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT(!jget(root, "command")); /* no spill onto the root table */
    ASSERT(strcmp(jget_str(jget(jget(root, "mcp_servers"), "ok"), "command"), "fine") == 0);
    yyjson_doc_free(doc);
    PASS();
}

SUITE(mcp_suite) {
    RUN_TEST(warm_start_does_not_block_and_catalog_appears);
    RUN_TEST(select_waits_for_warming_server);
    RUN_TEST(hung_server_does_not_stall);
    RUN_TEST(search_matches_tokens_not_substring);
    RUN_TEST(failed_server_silent_until_call);
    RUN_TEST(workspace_profile_never_loads_and_acp_never_warms);
    RUN_TEST(http_modern_json_warms_and_calls);
    RUN_TEST(http_sse_tools_call_fails_actionably);
    RUN_TEST(http_legacy_initialize_fallback);
    RUN_TEST(http_auth_from_env_and_rejects_literal_authorization);
    RUN_TEST(http_401_is_silent_until_named);

    RUN_TEST(import_off_by_default_then_merges_stdio);
    RUN_TEST(import_malformed_source_is_notice_not_fatal);
    RUN_TEST(import_project_configs_are_opt_in_and_attributed);
    RUN_TEST(import_missing_and_malformed_toml_are_notices);
    RUN_TEST(imported_stdio_server_runs_through_existing_permission_path);
    RUN_TEST(toml_subset_parses_mcp_servers_tables);
    RUN_TEST(toml_subset_survives_hostile_shapes);
}
