/* mcp.c — stdio JSONL MCP client. Server output is untrusted data.
 * Profile: ~/.tny/mcp.json {"servers":{"<name>":{"command":["…"]}}}.
 * Repo-local MCP files are never read (docs/features/mcp-and-skills.md). */
#include "mcp/mcp.h"
#include "util/util.h"
#include "util/tny_poll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

#define MCP_MAX_SERVERS 16
#define MCP_TIMEOUT_MS  30000

typedef struct {
    char *name;
    pid_t pid;
    int in_fd;  /* write requests here */
    int out_fd; /* read responses here */
    buf_t rbuf;
    int next_id;
    yyjson_doc *tools; /* cached tools/list result */
} mcp_server;

static mcp_server g_servers[MCP_MAX_SERVERS];
static int g_nservers = 0;

void mcp_shutdown_all(void) {
    for (int i = 0; i < g_nservers; i++) {
        mcp_server *s = &g_servers[i];
        if (s->pid > 0) {
            close(s->in_fd);
            close(s->out_fd);
            kill(s->pid, SIGTERM);
            waitpid(s->pid, NULL, WNOHANG);
        }
        free(s->name);
        buf_free(&s->rbuf);
        yyjson_doc_free(s->tools);
    }
    g_nservers = 0;
}

/* Send one JSON-RPC request and wait for the matching id. Returns doc. */
static yyjson_doc *rpc(mcp_server *s, const char *method, const char *params_json) {
    int id = ++s->next_id;
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}\n", id,
                method, params_json ? params_json : "{}");
    ssize_t w = write(s->in_fd, req.data, req.len);
    buf_free(&req);
    if (w < 0) return NULL;
    int64_t deadline = now_ms() + MCP_TIMEOUT_MS;
    for (;;) {
        /* scan buffered lines */
        for (;;) {
            char *nl = s->rbuf.len ? memchr(s->rbuf.data, '\n', s->rbuf.len) : NULL;
            if (!nl) break;
            size_t ll = (size_t)(nl - s->rbuf.data);
            yyjson_doc *doc = yyjson_read(s->rbuf.data, ll, 0);
            buf_consume(&s->rbuf, ll + 1);
            if (!doc) continue;
            yyjson_val *r = yyjson_doc_get_root(doc);
            if ((int)jget_int(r, "id", -1) == id) return doc;
            yyjson_doc_free(doc); /* notification or stale — ignore */
        }
        int left = (int)(deadline - now_ms());
        if (left <= 0) return NULL;
        struct pollfd pf = {s->out_fd, POLLIN, 0};
        if (tny_poll(&pf, 1, left) <= 0) return NULL;
        char tmp[16384];
        ssize_t n = read(s->out_fd, tmp, sizeof tmp);
        if (n <= 0) return NULL;
        buf_append(&s->rbuf, tmp, (size_t)n);
    }
}

static void notify(mcp_server *s, const char *method) {
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}\n", method);
    ssize_t w = write(s->in_fd, req.data, req.len);
    (void)w;
    buf_free(&req);
}

static mcp_server *find_server(const char *name) {
    for (int i = 0; i < g_nservers; i++)
        if (strcmp(g_servers[i].name, name) == 0) return &g_servers[i];
    return NULL;
}

/* Spawn + initialize one profile entry. */
static mcp_server *start_server(tools_env *env, const char *name, yyjson_val *conf) {
    if (g_nservers >= MCP_MAX_SERVERS) return NULL;
    yyjson_val *cmd = jget(conf, "command");
    if (!cmd || !yyjson_is_arr(cmd) || yyjson_arr_size(cmd) == 0) return NULL;

    char *argv[32] = {0};
    size_t idx, max;
    yyjson_val *v;
    int argc = 0;
    yyjson_arr_foreach(cmd, idx, max, v) {
        if (argc >= 31 || !yyjson_is_str(v)) break;
        argv[argc++] = (char *)yyjson_get_str(v);
    }

    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) return NULL;
    if (pipe(outpipe) != 0) {
        close(inpipe[0]);
        close(inpipe[1]);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) {
        dup2(inpipe[0], 0);
        dup2(outpipe[1], 1);
        /* stderr inherited: logs visible, protocol clean */
        close(inpipe[0]);
        close(inpipe[1]);
        close(outpipe[0]);
        close(outpipe[1]);
        if (chdir(env->ctx->cwd) != 0) _exit(127);
        if (!argv[0]) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(inpipe[0]);
    close(outpipe[1]);

    mcp_server *s = &g_servers[g_nservers++];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    s->pid = pid;
    s->in_fd = inpipe[1];
    s->out_fd = outpipe[0];
    buf_init(&s->rbuf);

    yyjson_doc *init = rpc(s, "initialize",
                           "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                           "\"clientInfo\":{\"name\":\"tny\",\"version\":\"" TNY_VERSION "\"}}");
    if (!init) {
        kill(pid, SIGTERM);
        close(s->in_fd);
        close(s->out_fd);
        free(s->name);
        g_nservers--;
        return NULL;
    }
    yyjson_doc_free(init);
    notify(s, "notifications/initialized");
    return s;
}

static yyjson_doc *load_profile(tools_env *env) {
    /* server mode (`tny acp`): the ACP client owns MCP — never read the
     * local profile (docs/backends/acp.md) */
    if (env->ctx->mcp_disabled) return NULL;
    char *file = path_join(env->ctx->tny_dir, "mcp.json");
    yyjson_doc *doc = jparse_file(file);
    free(file);
    return doc;
}

static mcp_server *get_server(tools_env *env, const char *name, char **err) {
    *err = NULL;
    mcp_server *s = find_server(name);
    if (s) return s;
    yyjson_doc *prof = load_profile(env);
    if (!prof) {
        *err = tool_err("no ~/.tny/mcp.json profile");
        return NULL;
    }
    yyjson_val *conf = jget(jget(yyjson_doc_get_root(prof), "servers"), name);
    if (!conf) {
        *err = tool_err("no MCP server named %s in ~/.tny/mcp.json", name);
        yyjson_doc_free(prof);
        return NULL;
    }
    s = start_server(env, name, conf);
    yyjson_doc_free(prof);
    if (!s) *err = tool_err("could not start MCP server %s", name);
    return s;
}

static yyjson_doc *server_tools(mcp_server *s) {
    if (!s->tools) {
        yyjson_doc *resp = rpc(s, "tools/list", "{}");
        if (resp) s->tools = resp;
    }
    return s->tools;
}

char *mcp_features(tools_env *env) {
    yyjson_doc *prof = load_profile(env);
    if (!prof)
        return xstrdup("no MCP servers configured (create ~/.tny/mcp.json with "
                       "{\"servers\":{\"name\":{\"command\":[\"…\"]}}})");
    yyjson_val *servers = jget(yyjson_doc_get_root(prof), "servers");
    buf_t out;
    buf_init(&out);
    if (servers && yyjson_is_obj(servers)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(servers, idx, max, k, v) {
            (void)v;
            const char *name = yyjson_get_str(k);
            if (!name) continue;
            mcp_server *s = find_server(name);
            buf_appendf(&out, "%s (%s)\n", name, s ? "connected" : "configured, not started");
        }
    }
    if (!out.len) buf_appends(&out, "(no MCP servers configured)");
    yyjson_doc_free(prof);
    return buf_detach(&out);
}

char *mcp_search_tools(tools_env *env, const char *query) {
    if (!query) return tool_err("missing query");
    yyjson_doc *prof = load_profile(env);
    if (!prof) return xstrdup("no MCP servers configured");
    yyjson_val *servers = jget(yyjson_doc_get_root(prof), "servers");
    buf_t out;
    buf_init(&out);
    if (servers && yyjson_is_obj(servers)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(servers, idx, max, k, v) {
            (void)v;
            const char *name = yyjson_get_str(k);
            if (!name) continue;
            char *err = NULL;
            mcp_server *s = get_server(env, name, &err);
            free(err);
            if (!s) continue;
            yyjson_doc *tl = server_tools(s);
            if (!tl) continue;
            yyjson_val *tools = jget(jget(yyjson_doc_get_root(tl), "result"), "tools");
            if (!tools || !yyjson_is_arr(tools)) continue;
            size_t i2, m2;
            yyjson_val *t;
            yyjson_arr_foreach(tools, i2, m2, t) {
                const char *tn = jget_str(t, "name");
                const char *td = jget_str(t, "description");
                if (!tn) continue;
                char lq[128], ln[256];
                snprintf(lq, sizeof lq, "%s", query);
                snprintf(ln, sizeof ln, "%s %s", tn, td ? td : "");
                for (char *p = lq; *p; p++) *p = (char)tolower((unsigned char)*p);
                for (char *p = ln; *p; p++) *p = (char)tolower((unsigned char)*p);
                if (strstr(ln, lq) || strlen(lq) == 0)
                    buf_appendf(&out, "%s/%s — %.140s\n", name, tn, td ? td : "");
            }
        }
    }
    yyjson_doc_free(prof);
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
    yyjson_doc *resp = rpc(s, "tools/call", params.data);
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
