/* tools_web.c — web_fetch and web_search (optional provider). */
#include "core/tools.h"
#include "util/tny_poll.h"
#include "net/net.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#define FETCH_MAX (1u * 1024u * 1024u)

static char *fetch_url(tools_env *env, const char *url, int redirects) {
    url_parts u;
    if (url_parse(url, &u) != 0 ||
        (strcmp(u.scheme, "http") != 0 && strcmp(u.scheme, "https") != 0))
        return tool_err("bad URL %s", url);
    char err[256];
    /* http_open takes a base URL; hand it scheme://host:port */
    buf_t base;
    buf_init(&base);
    buf_appendf(&base, "%s://%s:%d", u.scheme, u.host, u.port);
    http_conn *c = http_open(base.data, err, sizeof err);
    buf_free(&base);
    if (!c) return tool_err("%s", err);
    const char *hdrs[] = {"Accept: text/html, text/plain, application/json;q=0.9, */*;q=0.5", NULL};
    if (http_request(c, "GET", u.path, hdrs, NULL, 0) != 0) {
        http_close(c);
        return tool_err("request to %s failed", url);
    }
    int status = http_read_response(c, 30000);
    if (status >= 301 && status <= 308 && redirects > 0) {
        const char *loc = http_header(c, "Location");
        if (loc) {
            char *dup = xstrdup(loc);
            http_close(c);
            char *res = fetch_url(env, dup, redirects - 1);
            free(dup);
            return res;
        }
    }
    if (status < 0) {
        http_close(c);
        return tool_err("no response from %s", url);
    }
    buf_t body;
    buf_init(&body);
    int64_t deadline = now_ms() + 60000;
    for (;;) {
        char tmp[16384];
        ssize_t n = http_body_read(c, tmp, sizeof tmp);
        if (n == 0) break;
        if (n == -2) {
            if (now_ms() > deadline) break;
            struct pollfd pf = {http_fd(c), POLLIN, 0};
            tny_poll(&pf, 1, 1000);
            continue;
        }
        if (n < 0) break;
        if (body.len < FETCH_MAX) buf_append(&body, tmp, (size_t)n);
    }
    http_close(c);
    buf_t out;
    buf_init(&out);
    buf_appendf(&out, "HTTP %d from %s\n\n", status, url);
    buf_append(&out, body.data ? body.data : "", body.len);
    buf_free(&body);
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

bool tool_web_search_configured(tny_ctx *ctx) {
    if (!ctx) return false;
    return tny_settings_get_str(ctx, "web_search_command") != NULL ||
           tny_settings_get_str(ctx, "web_search_url") != NULL;
}

static void append_query_encoded(buf_t *out, const char *q) {
    for (const char *p = q; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.')
            buf_append(out, p, 1);
        else buf_appendf(out, "%%%02X", (unsigned char)*p);
    }
}

/* Replace every "{{query}}" and "{query}" in tmpl with the percent-encoded
 * query. The encoded form is safe inside a URL and inside a shell word: it
 * contains only [A-Za-z0-9-_.%]. */
char *tool_web_search_expand(const char *tmpl, const char *q) {
    buf_t out;
    buf_init(&out);
    const char *p = tmpl;
    for (;;) {
        const char *ph = strchr(p, '{');
        if (!ph) break;
        size_t plen = 0;
        if (strncmp(ph, "{{query}}", 9) == 0) plen = 9;
        else if (strncmp(ph, "{query}", 7) == 0) plen = 7;
        buf_append(&out, p, (size_t)(ph - p));
        if (plen) {
            append_query_encoded(&out, q);
            p = ph + plen;
        } else {
            buf_append(&out, "{", 1);
            p = ph + 1;
        }
    }
    buf_appends(&out, p);
    return buf_detach(&out);
}

#ifdef __EMSCRIPTEN__
static char *run_search_command(tools_env *env, const char *tmpl, const char *q) {
    (void)env;
    (void)tmpl;
    (void)q;
    return tool_err("web_search_command is not available in wasm; set web_search_url instead");
}
#else
/* Run the expanded command through the terminal tool's path so it inherits
 * the same cwd, --ssh remote, timeout, and bounded-output handling. */
static char *run_search_command(tools_env *env, const char *tmpl, const char *q) {
    char *cmd = tool_web_search_expand(tmpl, q);
    yyjson_mut_doc *mut = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *obj = mut ? yyjson_mut_obj(mut) : NULL;
    if (!obj) {
        yyjson_mut_doc_free(mut);
        free(cmd);
        return tool_err("out of memory");
    }
    yyjson_mut_obj_add_str(mut, obj, "command", cmd);
    yyjson_mut_obj_add_int(mut, obj, "timeout_s", 60);
    yyjson_mut_doc_set_root(mut, obj);
    char *json = jwrite(mut);
    yyjson_mut_doc_free(mut);
    free(cmd);
    if (!json) return tool_err("out of memory");
    yyjson_doc *doc = jparse(json, strlen(json));
    free(json);
    yyjson_val *args = doc ? yyjson_doc_get_root(doc) : NULL;
    bool handled = false;
    char *res = tool_ssh_execute(env, "terminal", args, &handled);
    if (!handled) res = tool_shell_execute(env, "terminal", args, &handled);
    yyjson_doc_free(doc);
    return res ? res : tool_err("web_search_command failed to run");
}
#endif

char *tool_web_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    *handled = true;
    if (strcmp(name, "web_fetch") == 0) {
        const char *url = jget_str(args, "url");
        if (!url) return tool_err("missing url");
        return fetch_url(env, url, 3);
    }
    if (strcmp(name, "web_search") == 0) {
        const char *q = jget_str(args, "query");
        if (!q) return tool_err("missing query");
        /* Optional providers (docs/adr/0055): "web_search_command" (a shell
         * command template, run like the terminal tool) beats
         * "web_search_url" (fetched over HTTP). Both take {query} or
         * {{query}} and receive the percent-encoded query. */
        const char *cmd_tmpl = tny_settings_get_str(env->ctx, "web_search_command");
        const char *url_tmpl = tny_settings_get_str(env->ctx, "web_search_url");
        if (!cmd_tmpl && !url_tmpl)
            return tool_err("no web search provider configured; set "
                            "\"web_search_command\" or \"web_search_url\" in "
                            "~/.tny/settings.json (a template containing {query}) "
                            "or use web_fetch");
        if (cmd_tmpl) return run_search_command(env, cmd_tmpl, q);
        char *url = tool_web_search_expand(url_tmpl, q);
        char *res = fetch_url(env, url, 3);
        free(url);
        return res;
    }
    *handled = false;
    return NULL;
}
