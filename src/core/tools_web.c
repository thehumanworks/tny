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
    if (status < 0) { http_close(c); return tool_err("no response from %s", url); }
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
        /* optional provider: settings "web_search_url" with {query} placeholder */
        const char *tmpl = tny_settings_get_str(env->ctx, "web_search_url");
        if (!tmpl)
            return tool_err("no web search provider configured; set "
                            "\"web_search_url\" in ~/.tny/settings.json "
                            "(a URL containing {query}) or use web_fetch");
        buf_t url;
        buf_init(&url);
        const char *ph = strstr(tmpl, "{query}");
        if (ph) {
            buf_append(&url, tmpl, (size_t)(ph - tmpl));
            for (const char *p = q; *p; p++) {
                if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
                    buf_append(&url, p, 1);
                else buf_appendf(&url, "%%%02X", (unsigned char)*p);
            }
            buf_appends(&url, ph + 7);
        } else {
            buf_appends(&url, tmpl);
        }
        char *res = fetch_url(env, url.data, 3);
        buf_free(&url);
        return res;
    }
    *handled = false;
    return NULL;
}
