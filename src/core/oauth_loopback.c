/* oauth_loopback.c — the loopback callback shared by the native ChatGPT
 * sign-ins (codex_login.c, docs/adr/0066; openai_login.c, docs/adr/0186).
 * One listener on 127.0.0.1, one request per accept, a small page back to
 * the browser. Codes and tokens never reach stdout/stderr. */
#include "core/oauth_loopback.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

static char *url_decode(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            char h[3] = {s[i + 1], s[i + 2], 0};
            char *end;
            long v = strtol(h, &end, 16);
            if (*end == 0 && h[0] && h[1]) {
                out[o++] = (char)v;
                i += 2;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

char *oauth_query_get(const char *query, size_t qlen, const char *key) {
    size_t klen = strlen(key);
    const char *p = query, *end = query + qlen;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *stop = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t)(stop - p));
        if (eq && (size_t)(eq - p) == klen && memcmp(p, key, klen) == 0)
            return url_decode(eq + 1, (size_t)(stop - eq - 1));
        p = stop + 1;
    }
    return NULL;
}

void oauth_open_browser(const char *url) {
    pid_t pid = fork();
    if (pid != 0) return;
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) close(devnull);
    }
#ifdef __APPLE__
    execlp("open", "open", url, (char *)NULL);
#else
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
    _exit(127);
}

void oauth_callback_free(oauth_callback *cb) {
    if (!cb) return;
    if (cb->code) secure_free(cb->code);
    free(cb->client_id);
    free(cb->error);
    memset(cb, 0, sizeof *cb);
}

oauth_callback_kind oauth_callback_parse(const char *query, size_t qlen,
                                         const oauth_callback_expect *e, oauth_callback *out,
                                         const char **why) {
    memset(out, 0, sizeof *out);
    *why = NULL;
    char *state = oauth_query_get(query, qlen, "state");
    char *error = oauth_query_get(query, qlen, "error");
    char *code = oauth_query_get(query, qlen, "code");
    char *client_id = oauth_query_get(query, qlen, "client_id");
    oauth_callback_kind kind = OAUTH_CALLBACK_NONE;
    if (!state || strcmp(state, e->state) != 0) {
        *why = "state mismatch";
    } else if (error) {
        *why = "sign-in refused";
        out->error = error;
        error = NULL;
        kind = OAUTH_CALLBACK_ERROR;
    } else if (!code || !*code) {
        *why = "missing authorization code";
    } else if (e->client_id && client_id && *client_id && strcmp(client_id, e->client_id) != 0) {
        /* reauthorization must not swap the saved registration */
        *why = "callback names a different client";
        out->error = xstrdup("client_id_mismatch");
        kind = OAUTH_CALLBACK_ERROR;
    } else if (e->require_client_id && (!client_id || !*client_id)) {
        *why = "missing issued client_id";
    } else {
        out->code = code;
        code = NULL;
        if (client_id && *client_id) {
            out->client_id = client_id;
            client_id = NULL;
        }
        kind = OAUTH_CALLBACK_CODE;
    }
    free(state);
    free(error);
    if (code) secure_free(code);
    free(client_id);
    return kind;
}

#ifndef __EMSCRIPTEN__
int oauth_loopback_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int oauth_loopback_port(int lfd) {
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    if (lfd < 0 || getsockname(lfd, (struct sockaddr *)&sa, &len) != 0) return -1;
    return ntohs(sa.sin_port);
}

static void http_reply(int fd, int status, const char *html) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                status, status == 200 ? "OK" : (status == 404 ? "Not Found" : "Bad Request"),
                strlen(html), html);
    size_t off = 0;
    while (off < b.len) {
        ssize_t w = write(fd, b.data + off, b.len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    buf_free(&b);
}

oauth_callback_kind oauth_loopback_serve(int lfd, const oauth_callback_expect *e,
                                         oauth_callback *out) {
    memset(out, 0, sizeof *out);
    if (lfd < 0) return OAUTH_CALLBACK_NONE;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return OAUTH_CALLBACK_NONE;
    char req[8192];
    memset(req, 0, sizeof req);
    size_t got = 0;
    int64_t deadline = now_ms() + 5000;
    while (got < sizeof req - 1) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (tny_poll(&pf, 1, 500) <= 0) {
            if (now_ms() > deadline) break;
            continue;
        }
        ssize_t n = read(fd, req + got, sizeof req - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[got] = 0;
    oauth_callback_kind kind = OAUTH_CALLBACK_NONE;
    size_t plen = strlen(e->path);
    const char *path = str_starts(req, "GET ") ? req + 4 : NULL;
    const char *sp = path ? strchr(path, ' ') : NULL;
    if (path && sp && str_starts(path, e->path) && (path[plen] == '?' || path + plen == sp)) {
        const char *q = memchr(path, '?', (size_t)(sp - path));
        const char *why = NULL;
        kind = q ? oauth_callback_parse(q + 1, (size_t)(sp - q - 1), e, out, &why)
                 : OAUTH_CALLBACK_NONE;
        if (kind == OAUTH_CALLBACK_CODE)
            http_reply(fd, 200,
                       "<h1>Signed in to tny</h1><p>You can close this tab and return to "
                       "the terminal.</p>");
        else if (kind == OAUTH_CALLBACK_ERROR)
            http_reply(fd, 400, "<h1>Sign-in refused</h1><p>See the terminal.</p>");
        else if (why && strcmp(why, "state mismatch") == 0)
            http_reply(fd, 400, "<h1>State mismatch</h1><p>Start the login again in tny.</p>");
        else http_reply(fd, 400, "<h1>Incomplete sign-in callback</h1><p>See the terminal.</p>");
    } else {
        http_reply(fd, 404, "<h1>Not found</h1>");
    }
    secure_zero(req, sizeof req);
    close(fd);
    return kind;
}
#else
int oauth_loopback_listen(int port) {
    (void)port;
    return -1;
}

int oauth_loopback_port(int lfd) {
    (void)lfd;
    return -1;
}

oauth_callback_kind oauth_loopback_serve(int lfd, const oauth_callback_expect *e,
                                         oauth_callback *out) {
    (void)lfd;
    (void)e;
    memset(out, 0, sizeof *out);
    return OAUTH_CALLBACK_NONE;
}
#endif
