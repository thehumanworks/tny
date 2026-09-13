/* tools_web.c — explicit search overrides and bounded DuckDuckGo fallback. */
#include "core/tools.h"
#include "util/tny_poll.h"
#include "net/net.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#define FETCH_MAX (1u * 1024u * 1024u)

static char *fetch_url(tools_env *env, const char *url, int redirects, bool raw) {
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
    const char *hdrs[] = {"Accept: text/html, text/plain, application/json;q=0.9, */*;q=0.5",
                          "User-Agent: tny/1.0 web-search", NULL};
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
            char *res = fetch_url(env, dup, redirects - 1, raw);
            free(dup);
            return res;
        }
    }
    if (status < 0 || (raw && (status < 200 || status >= 300))) {
        http_close(c);
        return tool_err("web request failed (HTTP %d)", status);
    }
    buf_t body;
    buf_init(&body);
    int64_t deadline = monotonic_ms() + (raw ? 20000 : 60000);
    const char *failure = NULL;
    for (;;) {
        if (env->control_pump) env->control_pump(env->control_pump_ud, 0);
        if (env->cancelled && env->cancelled(env->cancelled_ud)) {
            failure = "web request interrupted";
            break;
        }
        if (monotonic_ms() >= deadline) {
            if (raw) failure = "web request timed out";
            break;
        }
        char tmp[16384];
        ssize_t n = http_body_read(c, tmp, sizeof tmp);
        if (n == 0) break;
        if (n == -2) {
            struct pollfd pf = {http_fd(c), POLLIN, 0};
            tny_poll(&pf, 1, 100);
            continue;
        }
        if (n < 0) {
            if (raw) failure = "web response was interrupted";
            break;
        }
        if ((size_t)n > FETCH_MAX - body.len) {
            if (raw) {
                failure = "web response exceeds 1 MiB";
                break;
            }
            size_t keep = FETCH_MAX - body.len;
            if (keep) buf_append(&body, tmp, keep);
        } else buf_append(&body, tmp, (size_t)n);
        if (!raw && body.len >= FETCH_MAX) break;
    }
    http_close(c);
    if (failure) {
        buf_free(&body);
        return tool_err("%s", failure);
    }
    buf_t out;
    buf_init(&out);
    buf_appendf(&out, "HTTP %d from %s\n\n", status, url);
    buf_append(&out, body.data ? body.data : "", body.len);
    buf_free(&body);
    char *res = raw ? xstrndup(out.data, out.len) : tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

bool tool_web_search_configured(tny_ctx *ctx) { return ctx != NULL; }

bool tool_web_search_native(tny_ctx *ctx) {
    return ctx && tny_codex_chatgpt_mode(ctx) && !tny_wire_is_chat(ctx->wire_api) &&
           !tny_settings_get_str(ctx, "web_search_command") &&
           !tny_settings_get_str(ctx, "web_search_url");
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

/* Deliberately small HTML reader: only result anchors, never scripts or forms.
 * A changed page is an error, not invented empty search results. */
static void search_text(buf_t *out, const char *start, const char *end) {
    bool tag = false;
    for (const char *p = start; p < end; p++) {
        if (*p == '<') {
            tag = true;
            continue;
        }
        if (*p == '>') {
            tag = false;
            continue;
        }
        if (tag) continue;
        static const char *const entities[] = {"&amp;", "&quot;", "&#39;",
                                               "&lt;",  "&gt;",   "&nbsp;"};
        static const char decoded[] = "&\"'<> ";
        bool entity = false;
        for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++) {
            size_t n = strlen(entities[i]);
            if ((size_t)(end - p) >= n && strncmp(p, entities[i], n) == 0) {
                buf_append(out, &decoded[i], 1);
                p += n - 1;
                entity = true;
                break;
            }
        }
        if (!entity) {
            unsigned char c = (unsigned char)*p;
            if (c <= 32) {
                if (out->len && out->data[out->len - 1] != ' ') buf_appends(out, " ");
            } else if (c != 127) buf_append(out, p, 1);
        }
    }
}

static int search_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* DDG's redirect carries the real destination in uddg; never publish rut
 * tracking or decode an unrelated site's query string. */
static bool search_destination(buf_t *url) {
    url_parts u;
    if (!url->data || url_parse(url->data, &u) != 0) return false;
    if (strcmp(u.host, "duckduckgo.com") != 0 || !str_starts(u.path, "/l/?")) return true;
    const char *p = u.path + 4;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) >= 5 && strncmp(p, "uddg=", 5) == 0) {
            buf_t dest;
            buf_init(&dest);
            for (p += 5; p < end; p++) {
                unsigned char c = (unsigned char)*p;
                if (c == '%') {
                    if (end - p < 3 || search_hex(p[1]) < 0 || search_hex(p[2]) < 0) {
                        buf_free(&dest);
                        return false;
                    }
                    c = (unsigned char)(search_hex(p[1]) * 16 + search_hex(p[2]));
                    p += 2;
                }
                if (c < 32 || c == 127) {
                    buf_free(&dest);
                    return false;
                }
                buf_append(&dest, &c, 1);
            }
            if (!dest.data ||
                (!str_starts(dest.data, "https://") && !str_starts(dest.data, "http://"))) {
                buf_free(&dest);
                return false;
            }
            buf_free(url);
            *url = dest;
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

char *tool_web_search_parse_ddg(const char *html) {
    if (!html) return tool_err("DuckDuckGo returned no response");
    if (strstr(html, "anomaly.js") || strstr(html, "id=\"challenge-form\"") ||
        strstr(html, "id='challenge-form'"))
        return tool_err(
            "DuckDuckGo returned a bot challenge; configure web_search_url or web_search_command");
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "DuckDuckGo web search results:\n");
    int count = 0;
    const char *p = html;
    while (count < 10 && (p = strstr(p, "<a"))) {
        const char *end = strchr(p, '>');
        if (!end) break;
        const char *class = strstr(p, "result__a");
        const char *href = strstr(p, "href=");
        const char *close = strstr(end, "</a>");
        if (!class || class > end || !href || href > end || !close) {
            p = end + 1;
            continue;
        }
        href += 5;
        char quote = *href++;
        const char *he = strchr(href, quote);
        if ((quote != '\'' && quote != '"') || !he || he > end) {
            p = end + 1;
            continue;
        }
        buf_t url;
        buf_init(&url);
        if (he - href >= 2 && href[0] == '/' && href[1] == '/') buf_appends(&url, "https:");
        search_text(&url, href, he);
        if (!search_destination(&url) || !url.data ||
            (!str_starts(url.data, "https://") && !str_starts(url.data, "http://"))) {
            buf_free(&url);
            p = close + 4;
            continue;
        }
        buf_appendf(&out, "\n%d. ", ++count);
        search_text(&out, end + 1, close);
        buf_appendf(&out, "\n%s\n", url.data);
        buf_free(&url);
        const char *snippet = strstr(close, "result__snippet");
        const char *next = strstr(close, "result__a");
        if (snippet && (!next || snippet < next)) {
            const char *st = strchr(snippet, '>');
            const char *se = st ? strstr(st, "</") : NULL;
            if (st && se) {
                search_text(&out, st + 1, se);
                buf_appends(&out, "\n");
            }
        }
        p = close + 4;
    }
    if (!count) {
        buf_free(&out);
        if (strstr(html, "No results found") || strstr(html, "no-results"))
            return xstrdup("DuckDuckGo: no results found for this query.");
        return tool_err(
            "DuckDuckGo returned an unrecognized result page; search results unavailable");
    }
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
        return fetch_url(env, url, 3, false);
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
        if (!*q || strlen(q) > 4096) return tool_err("query must contain 1 to 4096 bytes");
        if (cmd_tmpl) return run_search_command(env, cmd_tmpl, q);
        bool fallback = !url_tmpl;
        if (fallback) url_tmpl = "https://html.duckduckgo.com/html/?q={query}";
        char *url = tool_web_search_expand(url_tmpl, q);
        char *res = fetch_url(env, url, 3, fallback);
        free(url);
        if (fallback && res && !str_starts(res, "error:")) {
            char *parsed = tool_web_search_parse_ddg(res);
            free(res);
            res = parsed;
        }
        return res;
    }
    *handled = false;
    return NULL;
}
