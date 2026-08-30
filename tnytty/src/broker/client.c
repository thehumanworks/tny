#include "broker/client.h"

#include "broker/broker.h"
#include "util/tt.h"

#include "yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_MS 5000
#define CLIENT_INPUT_MAX  (700u * 1024u)

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int write_all(int fd, const void *bytes, size_t len) {
    const char *p = bytes;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, CLIENT_TIMEOUT_MS) > 0) continue;
            errno = ETIMEDOUT;
        }
        return -1;
    }
    return 0;
}

static int send_request(int fd, const char *method, const char *path, const void *body,
                        size_t body_len) {
    char head[512];
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     method, path, body_len);
    if (n < 0 || (size_t)n >= sizeof head) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (write_all(fd, head, (size_t)n) != 0) return -1;
    return body_len ? write_all(fd, body, body_len) : 0;
}

static int receive_response(int fd, tt_http_response_parser *p) {
    char buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            int rc = tt_http_response_parser_feed(p, buf, (size_t)n);
            if (rc != 0) return rc;
            continue;
        }
        if (n == 0) {
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = {fd, POLLIN, 0};
            int rc = poll(&pfd, 1, CLIENT_TIMEOUT_MS);
            if (rc > 0) continue;
            errno = ETIMEDOUT;
        }
        return -1;
    }
}

static int request(tt_broker_client *c, const char *method, const char *path, const void *body,
                   size_t body_len, tt_http_response_parser *response) {
    int fd = connect_socket(c->socket_path);
    if (fd < 0) return -1;
    tt_http_response_parser_init(response);
    if (send_request(fd, method, path, body, body_len) != 0 ||
        receive_response(fd, response) != 1) {
        int saved = errno;
        close(fd);
        tt_http_response_parser_free(response);
        errno = saved;
        return -1;
    }
    close(fd);
    if (response->status < 200 || response->status >= 300) {
        errno = response->status == 404 ? ENOENT : response->status == 409 ? EBUSY : EIO;
        return -1;
    }
    return 0;
}

int tt_broker_client_open(tt_broker_client *c, const char *socket_path, char *err, size_t errcap) {
    memset(c, 0, sizeof *c);
    c->pending_fd = -1;
    char path[sizeof c->socket_path];
    if (!socket_path) {
        if (tt_broker_default_socket(path, sizeof path, err, errcap) != 0) return -1;
        socket_path = path;
    }
    if (strlen(socket_path) >= sizeof c->socket_path) {
        if (err && errcap) snprintf(err, errcap, "broker socket path is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(c->socket_path, sizeof c->socket_path, "%s", socket_path);
    return tt_broker_start(c->socket_path, 3000, err, errcap);
}

void tt_broker_client_close(tt_broker_client *c) {
    if (c->pending_fd >= 0) close(c->pending_fd);
    tt_http_response_parser_free(&c->pending);
    memset(c, 0, sizeof *c);
    c->pending_fd = -1;
}

static int simple_id_request(tt_broker_client *c, const char *method, const char *id,
                             const char *suffix) {
    if (!id || strlen(id) != 8) {
        errno = EINVAL;
        return -1;
    }
    char path[96];
    snprintf(path, sizeof path, "/v1/sessions/%s%s", id, suffix ? suffix : "");
    tt_http_response_parser response;
    int rc = request(c, method, path, NULL, 0, &response);
    tt_http_response_parser_free(&response);
    return rc;
}

int tt_broker_client_create(tt_broker_client *c, char *const argv[], const char *cwd, int cols,
                            int rows, char id[9]) {
    char *current_cwd = NULL;
    if (!cwd || !*cwd) {
        current_cwd = getcwd(NULL, 0);
        cwd = current_cwd;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        free(current_cwd);
        errno = ENOMEM;
        return -1;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        free(current_cwd);
        errno = ENOMEM;
        return -1;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "cols", cols);
    yyjson_mut_obj_add_int(doc, root, "rows", rows);
    if (cwd && *cwd) yyjson_mut_obj_add_str(doc, root, "cwd", cwd);
    if (argv && argv[0]) {
        yyjson_mut_val *cmd = yyjson_mut_arr(doc);
        for (int i = 0; argv[i]; i++) yyjson_mut_arr_add_str(doc, cmd, argv[i]);
        yyjson_mut_obj_add_val(doc, root, "cmd", cmd);
    }
    size_t body_len = 0;
    char *body = yyjson_mut_write(doc, 0, &body_len);
    yyjson_mut_doc_free(doc);
    free(current_cwd);
    if (!body) {
        errno = ENOMEM;
        return -1;
    }
    tt_http_response_parser response;
    int rc = request(c, "POST", "/v1/sessions", body, body_len, &response);
    free(body);
    if (rc == 0) {
        size_t len = 0;
        const unsigned char *raw = tt_http_response_body(&response, &len);
        yyjson_doc *parsed = yyjson_read((const char *)raw, len, 0);
        const char *value =
            parsed ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(parsed), "id")) : NULL;
        if (!value || strlen(value) != 8) {
            errno = EPROTO;
            rc = -1;
        } else {
            memcpy(id, value, 9);
        }
        if (parsed) yyjson_doc_free(parsed);
    }
    tt_http_response_parser_free(&response);
    return rc;
}

int tt_broker_client_attach(tt_broker_client *c, const char *id) {
    return simple_id_request(c, "POST", id, "/attach");
}

int tt_broker_client_detach(tt_broker_client *c, const char *id) {
    return simple_id_request(c, "POST", id, "/detach");
}

int tt_broker_client_kill(tt_broker_client *c, const char *id) {
    return simple_id_request(c, "DELETE", id, NULL);
}

int tt_broker_client_input(tt_broker_client *c, const char *id, const void *bytes, size_t len) {
    if (!id || strlen(id) != 8 || len > CLIENT_INPUT_MAX) {
        errno = len > CLIENT_INPUT_MAX ? EMSGSIZE : EINVAL;
        return -1;
    }
    size_t enc_len = (len + 2) / 3 * 4;
    char *body = malloc(enc_len + 16);
    if (!body) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(body, "{\"base64\":\"", 11);
    tt_base64_encode(body + 11, bytes, len);
    memcpy(body + 11 + enc_len, "\"}", 3);
    char path[96];
    snprintf(path, sizeof path, "/v1/sessions/%s/input", id);
    tt_http_response_parser response;
    int rc = request(c, "POST", path, body, enc_len + 13, &response);
    free(body);
    tt_http_response_parser_free(&response);
    return rc;
}

int tt_broker_client_resize(tt_broker_client *c, const char *id, int cols, int rows) {
    if (!id || strlen(id) != 8) {
        errno = EINVAL;
        return -1;
    }
    char body[96], path[96];
    int n = snprintf(body, sizeof body, "{\"cols\":%d,\"rows\":%d}", cols, rows);
    snprintf(path, sizeof path, "/v1/sessions/%s/resize", id);
    tt_http_response_parser response;
    int rc = request(c, "POST", path, body, (size_t)n, &response);
    tt_http_response_parser_free(&response);
    return rc;
}

int tt_broker_client_list(tt_broker_client *c, tt_buf *json) {
    tt_buf_init(json);
    tt_http_response_parser response;
    if (request(c, "GET", "/v1/sessions", NULL, 0, &response) != 0) {
        tt_http_response_parser_free(&response);
        return -1;
    }
    size_t len = 0;
    const unsigned char *body = tt_http_response_body(&response, &len);
    int rc = tt_buf_append(json, body, len) ? 0 : -1;
    if (rc != 0) errno = ENOMEM;
    tt_http_response_parser_free(&response);
    return rc;
}

int tt_broker_client_snapshot(tt_broker_client *c, const char *id, int scrollback, tt_buf *body) {
    tt_buf_init(body);
    if (!id || strlen(id) != 8) {
        errno = EINVAL;
        return -1;
    }
    char path[128];
    snprintf(path, sizeof path, "/v1/sessions/%s/screen?format=wire&scrollback=%d", id,
             scrollback < 0 ? 0 : scrollback);
    tt_http_response_parser response;
    if (request(c, "GET", path, NULL, 0, &response) != 0) {
        tt_http_response_parser_free(&response);
        return -1;
    }
    size_t len = 0;
    const unsigned char *raw = tt_http_response_body(&response, &len);
    if (!tt_buf_append(body, raw, len)) {
        tt_http_response_parser_free(&response);
        errno = ENOMEM;
        return -1;
    }
    tt_http_response_parser_free(&response);
    return 0;
}

int tt_broker_client_snapshot_begin(tt_broker_client *c, const char *id, int scrollback) {
    if (c->pending_fd >= 0) {
        errno = EBUSY;
        return -1;
    }
    if (!id || strlen(id) != 8) {
        errno = EINVAL;
        return -1;
    }
    int fd = connect_socket(c->socket_path);
    if (fd < 0) return -1;
    char path[128];
    snprintf(path, sizeof path, "/v1/sessions/%s/screen?format=wire&scrollback=%d", id,
             scrollback < 0 ? 0 : scrollback);
    if (send_request(fd, "GET", path, NULL, 0) != 0) {
        close(fd);
        return -1;
    }
    tt_http_response_parser_free(&c->pending);
    tt_http_response_parser_init(&c->pending);
    c->pending_fd = fd;
    return 0;
}

int tt_broker_client_pollfd(const tt_broker_client *c, struct pollfd *out) {
    if (c->pending_fd < 0) return 0;
    out->fd = c->pending_fd;
    out->events = POLLIN;
    out->revents = 0;
    return 1;
}

int tt_broker_client_pump(tt_broker_client *c, short revents, const void **body, size_t *len) {
    if (c->pending_fd < 0) return 0;
    if (revents & (POLLERR | POLLNVAL)) {
        errno = EIO;
        goto fail;
    }
    char buf[16384];
    for (;;) {
        ssize_t n = read(c->pending_fd, buf, sizeof buf);
        if (n > 0) {
            int rc = tt_http_response_parser_feed(&c->pending, buf, (size_t)n);
            if (rc < 0) goto fail;
            if (rc > 0) {
                close(c->pending_fd);
                c->pending_fd = -1;
                if (c->pending.status != 200) {
                    errno = c->pending.status == 404 ? ENOENT : EIO;
                    return -1;
                }
                size_t body_len = 0;
                const unsigned char *snapshot = tt_http_response_body(&c->pending, &body_len);
                if (body) *body = snapshot;
                if (len) *len = body_len;
                return 1;
            }
            continue;
        }
        if (n == 0) {
            errno = EPROTO;
            goto fail;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        goto fail;
    }
fail:
    close(c->pending_fd);
    c->pending_fd = -1;
    return -1;
}
