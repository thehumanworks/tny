/* http.c — REST router + minimal HTTP/1.1 server (docs/http-api.md). */
#include "api/http.h"

#include "picohttpparser.h"
#include "yyjson.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CONNS   32
#define MAX_HEADERS 32
#define MAX_REQUEST (1 << 20) /* 1 MiB: input bodies are small JSON */

/* ---- response scaffolding ------------------------------------------- */

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static void respond(tt_buf *out, int code, const char *ctype, const char *body, size_t blen) {
    tt_buf_appendf(out,
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\n\r\n",
                   code, status_text(code), ctype, blen);
    tt_buf_append(out, body, blen);
}

static void respond_json_doc(tt_buf *out, int code, yyjson_mut_doc *doc) {
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    if (!json) {
        tt_api_error(out, 500, "out of memory");
        return;
    }
    respond(out, code, "application/json", json, len);
    free(json);
}

void tt_api_error(tt_buf *out, int status, const char *message) {
    char body[256];
    int n = snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    respond(out, status, "application/json", body, (size_t)n);
}

bool tt_api_auth_ok(const tt_api *api, const char *auth) {
    if (!api->token || !api->token[0]) return true;
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) return false;
    return tt_const_eq(auth + 7, api->token);
}

/* ---- JSON builders --------------------------------------------------- */

static yyjson_mut_val *session_obj(yyjson_mut_doc *d, const tt_session *s) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, o, "id", s->id);
    yyjson_mut_val *cmd = yyjson_mut_arr(d);
    for (char **a = s->argv; *a; a++) yyjson_mut_arr_add_str(d, cmd, *a);
    yyjson_mut_obj_add_val(d, o, "cmd", cmd);
    yyjson_mut_obj_add_int(d, o, "cols", vt_cols(s->term));
    yyjson_mut_obj_add_int(d, o, "rows", vt_rows(s->term));
    yyjson_mut_obj_add_str(d, o, "title", vt_title(s->term));
    yyjson_mut_obj_add_bool(d, o, "alive", s->alive);
    if (s->alive) yyjson_mut_obj_add_null(d, o, "exit_code");
    else yyjson_mut_obj_add_int(d, o, "exit_code", s->exit_code);
    yyjson_mut_obj_add_int(d, o, "created_unix", (int64_t)s->created);
    yyjson_mut_obj_add_int(d, o, "graphics", vt_graphics_count(s->term));
    return o;
}

static void color_str(uint32_t c, char *out, size_t cap) {
    if (VT_COLOR_TAG(c) == VT_COLOR_IDX) snprintf(out, cap, "@%u", VT_COLOR_VAL(c));
    else if (VT_COLOR_TAG(c) == VT_COLOR_RGB) snprintf(out, cap, "#%06x", VT_COLOR_VAL(c));
    else snprintf(out, cap, "%s", "");
}

static yyjson_mut_val *attrs_arr(yyjson_mut_doc *d, uint16_t a) {
    static const struct {
        uint16_t bit;
        const char *name;
    } names[] = {
        {VT_ATTR_BOLD, "bold"},           {VT_ATTR_FAINT, "faint"},   {VT_ATTR_ITALIC, "italic"},
        {VT_ATTR_UNDERLINE, "underline"}, {VT_ATTR_BLINK, "blink"},   {VT_ATTR_REVERSE, "reverse"},
        {VT_ATTR_HIDDEN, "hidden"},       {VT_ATTR_STRIKE, "strike"},
    };
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (a & names[i].bit) yyjson_mut_arr_add_str(d, arr, names[i].name);
    return arr;
}

static void screen_json(tt_api *api, tt_session *s, tt_buf *out) {
    (void)api;
    vt *t = s->term;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_int(d, root, "cols", vt_cols(t));
    yyjson_mut_obj_add_int(d, root, "rows", vt_rows(t));
    yyjson_mut_val *cur = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, cur, "x", vt_cursor_x(t));
    yyjson_mut_obj_add_int(d, cur, "y", vt_cursor_y(t));
    yyjson_mut_obj_add_bool(d, cur, "visible", vt_cursor_visible(t));
    yyjson_mut_obj_add_val(d, root, "cursor", cur);
    yyjson_mut_obj_add_bool(d, root, "alt_screen", vt_alt_screen(t));
    yyjson_mut_obj_add_str(d, root, "title", vt_title(t));
    yyjson_mut_obj_add_int(d, root, "scrollback", vt_scrollback_len(t));

    size_t text_cap = (size_t)vt_cols(t) * 8 + 1;
    char *text = malloc(text_cap);
    yyjson_mut_val *lines = yyjson_mut_arr(d);
    for (int y = 0; text && y < vt_rows(t); y++) {
        yyjson_mut_val *line = yyjson_mut_obj(d);
        vt_line_text(t, y, text, text_cap);
        yyjson_mut_obj_add_strcpy(d, line, "text", text);
        /* styled runs over the padded row (blank cells carry bg color) */
        yyjson_mut_val *runs = yyjson_mut_arr(d);
        const vt_cell *cells = vt_line(t, y);
        int x = 0;
        while (x < vt_cols(t)) {
            uint16_t a = cells[x].attrs & (uint16_t)~(VT_ATTR_WIDE | VT_ATTR_WIDE_CONT);
            uint32_t fg = cells[x].fg, bg = cells[x].bg;
            int start = x;
            while (x < vt_cols(t) && fg == cells[x].fg && bg == cells[x].bg &&
                   a == (cells[x].attrs & (uint16_t)~(VT_ATTR_WIDE | VT_ATTR_WIDE_CONT)))
                x++;
            if (a || fg || bg) {
                yyjson_mut_val *run = yyjson_mut_obj(d);
                yyjson_mut_obj_add_int(d, run, "start", start);
                yyjson_mut_obj_add_int(d, run, "len", x - start);
                char col[16];
                color_str(fg, col, sizeof col);
                yyjson_mut_obj_add_strcpy(d, run, "fg", col);
                color_str(bg, col, sizeof col);
                yyjson_mut_obj_add_strcpy(d, run, "bg", col);
                yyjson_mut_obj_add_val(d, run, "attrs", attrs_arr(d, a));
                yyjson_mut_arr_add_val(runs, run);
            }
        }
        yyjson_mut_obj_add_val(d, line, "runs", runs);
        yyjson_mut_arr_add_val(lines, line);
    }
    free(text);
    yyjson_mut_obj_add_val(d, root, "lines", lines);
    respond_json_doc(out, 200, d);
    yyjson_mut_doc_free(d);
}

static void screen_text(tt_session *s, tt_buf *out) {
    vt *t = s->term;
    size_t cap = (size_t)vt_cols(t) * 8 + 2;
    char *text = malloc(cap);
    if (!text) {
        tt_api_error(out, 500, "out of memory");
        return;
    }
    tt_buf body;
    tt_buf_init(&body);
    for (int y = 0; y < vt_rows(t); y++) {
        size_t n = vt_line_text(t, y, text, cap);
        text[n] = '\n';
        tt_buf_append(&body, text, n + 1);
    }
    free(text);
    if (body.oom) tt_api_error(out, 500, "out of memory");
    else respond(out, 200, "text/plain; charset=utf-8", body.data ? body.data : "", body.len);
    tt_buf_free(&body);
}

/* ---- route handlers -------------------------------------------------- */

static void handle_health(tt_api *api, tt_buf *out) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_bool(d, root, "ok", true);
    yyjson_mut_obj_add_str(d, root, "version", api->version);
    yyjson_mut_obj_add_int(d, root, "sessions", api->reg->count);
    respond_json_doc(out, 200, d);
    yyjson_mut_doc_free(d);
}

static void handle_list(tt_api *api, tt_buf *out) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (tt_session *s = api->reg->head; s; s = s->next)
        yyjson_mut_arr_add_val(arr, session_obj(d, s));
    yyjson_mut_obj_add_val(d, root, "sessions", arr);
    respond_json_doc(out, 200, d);
    yyjson_mut_doc_free(d);
}

static void handle_create(tt_api *api, const char *body, size_t body_len, tt_buf *out) {
    int cols = 80, rows = 24;
    char **argv = NULL;
    size_t argc = 0;
    if (body_len) {
        yyjson_doc *doc = yyjson_read(body, body_len, 0);
        if (!doc) {
            tt_api_error(out, 400, "invalid JSON body");
            return;
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *v = yyjson_obj_get(root, "cols");
        if (yyjson_is_int(v)) cols = (int)yyjson_get_int(v);
        v = yyjson_obj_get(root, "rows");
        if (yyjson_is_int(v)) rows = (int)yyjson_get_int(v);
        yyjson_val *cmd = yyjson_obj_get(root, "cmd");
        if (yyjson_is_arr(cmd) && yyjson_arr_size(cmd) > 0) {
            argc = yyjson_arr_size(cmd);
            argv = calloc(argc + 1, sizeof *argv);
            size_t i, ok = argv != NULL;
            yyjson_val *el;
            yyjson_arr_foreach(cmd, i, argc, el) {
                if (!ok) break;
                if (!yyjson_is_str(el) || !(argv[i] = strdup(yyjson_get_str(el)))) ok = 0;
            }
            if (!ok) {
                if (argv)
                    for (size_t j = 0; argv[j]; j++) free(argv[j]);
                free(argv);
                yyjson_doc_free(doc);
                tt_api_error(out, 400, "cmd must be an array of strings");
                return;
            }
        }
        yyjson_doc_free(doc);
    }
    if (cols < 1 || cols > 1000 || rows < 1 || rows > 1000) {
        if (argv)
            for (size_t j = 0; argv[j]; j++) free(argv[j]);
        free(argv);
        tt_api_error(out, 400, "cols/rows out of range");
        return;
    }
    tt_session *s = tt_session_create(api->reg, argv, cols, rows);
    if (argv)
        for (size_t j = 0; argv[j]; j++) free(argv[j]);
    free(argv);
    if (!s) {
        tt_api_error(out, 500, "failed to spawn session");
        return;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(d, session_obj(d, s));
    respond_json_doc(out, 201, d);
    yyjson_mut_doc_free(d);
}

static void handle_input(tt_api *api, tt_session *s, const char *body, size_t body_len,
                         tt_buf *out) {
    (void)api;
    yyjson_doc *doc = body_len ? yyjson_read(body, body_len, 0) : NULL;
    if (!doc) {
        tt_api_error(out, 400, "invalid JSON body");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *text = yyjson_obj_get(root, "text");
    yyjson_val *b64 = yyjson_obj_get(root, "base64");
    int written = -1;
    if (yyjson_is_str(text)) {
        written = tt_session_write(s, yyjson_get_str(text), yyjson_get_len(text));
    } else if (yyjson_is_str(b64)) {
        /* small inline base64 decoder (input only) */
        const char *src = yyjson_get_str(b64);
        size_t slen = yyjson_get_len(b64);
        unsigned char *raw = malloc(slen / 4 * 3 + 3);
        if (raw) {
            size_t o = 0;
            unsigned int acc = 0;
            int bits = 0;
            bool bad = false;
            for (size_t i = 0; i < slen && !bad; i++) {
                char ch = src[i];
                int v;
                if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
                else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
                else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
                else if (ch == '+') v = 62;
                else if (ch == '/') v = 63;
                else if (ch == '=') break;
                else {
                    bad = true;
                    break;
                }
                acc = (acc << 6) | (unsigned)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    raw[o++] = (unsigned char)(acc >> bits);
                }
            }
            if (bad) {
                free(raw);
                yyjson_doc_free(doc);
                tt_api_error(out, 400, "invalid base64");
                return;
            }
            written = tt_session_write(s, (const char *)raw, o);
            free(raw);
        }
    } else {
        yyjson_doc_free(doc);
        tt_api_error(out, 400, "body needs \\\"text\\\" or \\\"base64\\\"");
        return;
    }
    yyjson_doc_free(doc);
    if (written < 0) {
        if (errno == ENOBUFS) {
            /* The child is not draining its input; queueing more would
             * grow without bound, so reject the whole write rather than
             * accept a truncated prefix (docs/http-api.md). */
            tt_api_error(out, 503, "input queue full");
            return;
        }
        tt_api_error(out, 500, "session is not writable");
        return;
    }
    char b[64];
    int n = snprintf(b, sizeof b, "{\"written\":%d}", written);
    respond(out, 200, "application/json", b, (size_t)n);
}

static void handle_resize(tt_api *api, tt_session *s, const char *body, size_t body_len,
                          tt_buf *out) {
    (void)api;
    yyjson_doc *doc = body_len ? yyjson_read(body, body_len, 0) : NULL;
    if (!doc) {
        tt_api_error(out, 400, "invalid JSON body");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    int cols = (int)yyjson_get_int(yyjson_obj_get(root, "cols"));
    int rows = (int)yyjson_get_int(yyjson_obj_get(root, "rows"));
    yyjson_doc_free(doc);
    if (cols < 1 || cols > 1000 || rows < 1 || rows > 1000) {
        tt_api_error(out, 400, "cols/rows out of range");
        return;
    }
    tt_session_resize(s, cols, rows);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(d, session_obj(d, s));
    respond_json_doc(out, 200, d);
    yyjson_mut_doc_free(d);
}

/* ---- router ---------------------------------------------------------- */

void tt_api_route(tt_api *api, const char *method, const char *path, const char *body,
                  size_t body_len, tt_buf *out) {
    /* split off the query string */
    char pathbuf[256];
    snprintf(pathbuf, sizeof pathbuf, "%s", path);
    char *query = strchr(pathbuf, '?');
    if (query) *query++ = '\0';

    bool get = strcmp(method, "GET") == 0;
    bool post = strcmp(method, "POST") == 0;
    bool del = strcmp(method, "DELETE") == 0;

    if (strcmp(pathbuf, "/v1/health") == 0) {
        if (!get) tt_api_error(out, 405, "method not allowed");
        else handle_health(api, out);
        return;
    }
    if (strcmp(pathbuf, "/v1/sessions") == 0) {
        if (get) handle_list(api, out);
        else if (post) handle_create(api, body, body_len, out);
        else tt_api_error(out, 405, "method not allowed");
        return;
    }
    if (strncmp(pathbuf, "/v1/sessions/", 13) == 0) {
        char *rest = pathbuf + 13;
        char *slash = strchr(rest, '/');
        const char *sub = NULL;
        if (slash) {
            *slash = '\0';
            sub = slash + 1;
        }
        tt_session *s = tt_session_find(api->reg, rest);
        if (!s) {
            tt_api_error(out, 404, "no such session");
            return;
        }
        if (!sub) {
            if (get) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                yyjson_mut_doc_set_root(d, session_obj(d, s));
                respond_json_doc(out, 200, d);
                yyjson_mut_doc_free(d);
            } else if (del) {
                tt_session_destroy(api->reg, s);
                respond(out, 200, "application/json", "{\"ok\":true}", 11);
            } else {
                tt_api_error(out, 405, "method not allowed");
            }
            return;
        }
        if (strcmp(sub, "screen") == 0 && get) {
            if (query && strstr(query, "format=json")) screen_json(api, s, out);
            else screen_text(s, out);
            return;
        }
        if (strcmp(sub, "input") == 0 && post) {
            handle_input(api, s, body, body_len, out);
            return;
        }
        if (strcmp(sub, "resize") == 0 && post) {
            handle_resize(api, s, body, body_len, out);
            return;
        }
        tt_api_error(out, 404, "no such route");
        return;
    }
    tt_api_error(out, 404, "no such route");
}

/* ---- socket server --------------------------------------------------- */

typedef struct {
    int fd;
    tt_buf in;
    tt_buf out;
    size_t out_off;
} conn;

struct tt_http {
    tt_api *api;
    int listen_fd;
    conn conns[MAX_CONNS];
};

static bool host_is_loopback(const char *host) {
    return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
           strcmp(host, "localhost") == 0;
}

tt_http *tt_http_listen(tt_api *api, const char *host, int port, char *err, size_t errlen) {
    if (!host_is_loopback(host) && (!api->token || !api->token[0])) {
        snprintf(err, errlen, "refusing non-loopback bind without --token (docs/adr/0002)");
        return NULL;
    }
    struct in_addr addr;
    const char *resolved = strcmp(host, "localhost") == 0 ? "127.0.0.1" : host;
    if (inet_pton(AF_INET, resolved, &addr) != 1) {
        snprintf(err, errlen, "listen host must be an IPv4 address (got %s)", host);
        return NULL;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr = addr;
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        snprintf(err, errlen, "bind %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    tt_http *h = calloc(1, sizeof *h);
    if (!h) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    h->api = api;
    h->listen_fd = fd;
    for (int i = 0; i < MAX_CONNS; i++) h->conns[i].fd = -1;
    return h;
}

static void conn_close(conn *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    tt_buf_free(&c->in);
    tt_buf_free(&c->out);
    c->out_off = 0;
}

void tt_http_free(tt_http *h) {
    if (!h) return;
    for (int i = 0; i < MAX_CONNS; i++) conn_close(&h->conns[i]);
    if (h->listen_fd >= 0) close(h->listen_fd);
    free(h);
}

int tt_http_fill(tt_http *h, struct pollfd *fds, int max) {
    int n = 0;
    if (n < max) {
        fds[n].fd = h->listen_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    for (int i = 0; i < MAX_CONNS && n < max; i++) {
        if (h->conns[i].fd < 0) continue;
        fds[n].fd = h->conns[i].fd;
        fds[n].events = h->conns[i].out.len ? POLLOUT : POLLIN;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

/* A request is complete when headers parse and the whole body arrived. */
static void conn_try_respond(tt_http *h, conn *c) {
    const char *method, *path;
    size_t method_len, path_len;
    int minor;
    struct phr_header headers[MAX_HEADERS];
    size_t nheaders = MAX_HEADERS;
    int hlen = phr_parse_request(c->in.data, c->in.len, &method, &method_len, &path, &path_len,
                                 &minor, headers, &nheaders, 0);
    if (hlen == -2) {
        if (c->in.len > MAX_REQUEST) conn_close(c);
        return; /* incomplete */
    }
    if (hlen < 0) {
        tt_api_error(&c->out, 400, "malformed request");
        return;
    }
    size_t content_len = 0;
    const char *auth = NULL;
    char authbuf[256] = "";
    for (size_t i = 0; i < nheaders; i++) {
        if (headers[i].name_len == 14 && strncasecmp(headers[i].name, "Content-Length", 14) == 0)
            content_len = strtoul(headers[i].value, NULL, 10);
        if (headers[i].name_len == 13 && strncasecmp(headers[i].name, "Authorization", 13) == 0) {
            size_t vlen = headers[i].value_len < sizeof authbuf - 1 ? headers[i].value_len
                                                                    : sizeof authbuf - 1;
            memcpy(authbuf, headers[i].value, vlen);
            authbuf[vlen] = '\0';
            auth = authbuf;
        }
    }
    if (content_len > MAX_REQUEST) {
        tt_api_error(&c->out, 400, "body too large");
        return;
    }
    if (c->in.len < (size_t)hlen + content_len) return; /* body still arriving */

    char methodbuf[16], pathbuf[256];
    snprintf(methodbuf, sizeof methodbuf, "%.*s", (int)method_len, method);
    snprintf(pathbuf, sizeof pathbuf, "%.*s", (int)path_len, path);
    if (!tt_api_auth_ok(h->api, auth)) tt_api_error(&c->out, 401, "unauthorized");
    else tt_api_route(h->api, methodbuf, pathbuf, c->in.data + hlen, content_len, &c->out);
}

void tt_http_handle(tt_http *h, const struct pollfd *fds, int n) {
    int idx = 0;
    if (idx < n && fds[idx].fd == h->listen_fd) {
        if (fds[idx].revents & POLLIN) {
            for (;;) {
                int cfd = accept(h->listen_fd, NULL, NULL);
                if (cfd < 0) break;
                fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
                int slot = -1;
                for (int i = 0; i < MAX_CONNS; i++)
                    if (h->conns[i].fd < 0) {
                        slot = i;
                        break;
                    }
                if (slot < 0) {
                    close(cfd); /* full house: shed load */
                    continue;
                }
                conn *c = &h->conns[slot];
                c->fd = cfd;
                tt_buf_init(&c->in);
                tt_buf_init(&c->out);
                c->out_off = 0;
            }
        }
        idx++;
    }
    for (; idx < n; idx++) {
        conn *c = NULL;
        for (int i = 0; i < MAX_CONNS; i++)
            if (h->conns[i].fd == fds[idx].fd) {
                c = &h->conns[i];
                break;
            }
        if (!c) continue;
        if (fds[idx].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* flush what we can on HUP-with-data; otherwise drop */
            if (!(fds[idx].revents & POLLIN) && !c->out.len) {
                conn_close(c);
                continue;
            }
        }
        if (fds[idx].revents & POLLIN) {
            char tmp[8192];
            for (;;) {
                ssize_t r = read(c->fd, tmp, sizeof tmp);
                if (r > 0) {
                    tt_buf_append(&c->in, tmp, (size_t)r);
                    continue;
                }
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (r < 0 && errno == EINTR) continue;
                /* peer closed before a full request */
                if (!c->out.len) {
                    conn_close(c);
                    c = NULL;
                }
                break;
            }
            if (!c) continue;
            if (c->in.oom) {
                conn_close(c);
                continue;
            }
            if (!c->out.len) conn_try_respond(h, c);
        }
        if (c->out.len) {
            while (c->out_off < c->out.len) {
                ssize_t w = write(c->fd, c->out.data + c->out_off, c->out.len - c->out_off);
                if (w > 0) {
                    c->out_off += (size_t)w;
                    continue;
                }
                if (w < 0 && errno == EINTR) continue;
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; /* POLLOUT */
                conn_close(c); /* peer gone (EPIPE et al.): drop the response */
                break;
            }
            if (c->fd >= 0 && c->out_off >= c->out.len) conn_close(c); /* Connection: close */
        }
    }
}
