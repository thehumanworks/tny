/* cmd_control.c — tool-role clients for the session control channel.
 *
 * These verbs deliberately create no tny_ctx and never consult /dev/tty.
 * A subprocess launched by `terminal` receives the runner's resolved socket
 * in TNY_SESSION_SOCK, handshakes as role `tool`, then performs one correlated
 * NDJSON request (ADR 0057, ADR 0058).
 *
 * The exchange itself is a private data-returning primitive
 * (tny_control_request, docs/adr/0096): it prints nothing and exits nothing on
 * any platform, including the WebAssembly branch. The command wrappers at the
 * bottom own every message and exit code. */
#include "cli/cmd_control.h"

#include "json/json.h"
#include "net/net.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTROL_MAX_LINE   (1024u * 1024u)
#define CONTROL_CONNECT_MS 1500

/* Explicit per-op wire name. Never a two-way ternary: a new op must not be
 * able to fall back to manual image attachment. */
static const char *control_op_name(tny_control_op op) {
    switch (op) {
    case TNY_CONTROL_OP_ASK_USER: return "ask_user";
    case TNY_CONTROL_OP_IMAGE_ATTACH: return "image_attach";
    case TNY_CONTROL_OP_IMAGE_PREVIEW: return "image_preview";
    }
    return "";
}

void tny_control_reply_free(tny_control_reply *reply) {
    if (!reply) return;
    free(reply->id);
    free(reply->answer);
    free(reply->error);
    free(reply->status);
    free(reply->error_code);
    memset(reply, 0, sizeof *reply);
}

#ifndef __EMSCRIPTEN__ /* wasm has no session socket: the helpers below are unused there */
static volatile sig_atomic_t g_control_interrupted;

static void control_on_sigint(int sig) {
    (void)sig;
    g_control_interrupted = 1;
}

static int control_connect(const char *path) {
    int64_t deadline = monotonic_ms() + CONTROL_CONNECT_MS;
    for (;;) {
        int fd = unix_connect(path);
        if (fd >= 0) return fd;
        if (monotonic_ms() >= deadline) return -1;
        struct pollfd none = {-1, 0, 0};
        tny_poll(&none, 1, 50);
    }
}

static int control_write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        if (g_control_interrupted) return -2;
        ssize_t n = socket_write(fd, data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {fd, POLLOUT, 0};
            int pr = tny_poll(&pf, 1, 200);
            if (pr >= 0) continue;
            if (errno == EINTR) continue;
        }
        return -1;
    }
    return 0;
}

/* Serialize one request. Each op names its own fields explicitly. */
static int control_build_request(tny_control_op op, const char *payload,
                                 const char *expected_sha256, uint64_t expected_bytes,
                                 const char *id, buf_t *wire) {
    buf_appends(wire, "{\"op\":\"hello\",\"role\":\"tool\"}\n{\"op\":");
    jescape(wire, control_op_name(op));
    buf_appends(wire, ",\"id\":");
    jescape(wire, id);
    bool known = false;
    switch (op) {
    case TNY_CONTROL_OP_ASK_USER:
        buf_appends(wire, ",\"question\":");
        jescape(wire, payload ? payload : "");
        known = true;
        break;
    case TNY_CONTROL_OP_IMAGE_ATTACH:
        buf_appends(wire, ",\"path\":");
        jescape(wire, payload ? payload : "");
        known = true;
        break;
    case TNY_CONTROL_OP_IMAGE_PREVIEW:
        buf_appends(wire, ",\"path\":");
        jescape(wire, payload ? payload : "");
        buf_appends(wire, ",\"expected_sha256\":");
        jescape(wire, expected_sha256 ? expected_sha256 : "");
        if (expected_bytes)
            buf_appendf(wire, ",\"expected_bytes\":%llu", (unsigned long long)expected_bytes);
        known = true;
        break;
    }
    if (!known) return -1;
    buf_appends(wire, "}\n");
    return 0;
}

static char *control_take_str(yyjson_val *root, const char *field) {
    yyjson_val *value = jget(root, field);
    if (!yyjson_is_str(value)) return NULL;
    const char *str = yyjson_get_str(value);
    return memchr(str, '\0', yyjson_get_len(value)) ? NULL : xstrdup(str);
}

/* Consume one reply line. Returns -1 for an unrelated message (keep reading),
 * 0 when this exchange's reply was recorded. */
static int control_take_reply(tny_control_op op, const char *id, yyjson_val *root,
                              tny_control_reply *reply) {
    yyjson_val *rid = jget(root, "id");
    if (!yyjson_is_str(rid) || yyjson_get_len(rid) != strlen(id) ||
        memcmp(yyjson_get_str(rid), id, strlen(id)) != 0)
        return -1;
    reply->error = control_take_str(root, "error");
    reply->answer = control_take_str(root, "answer");
    reply->status = control_take_str(root, "status");
    reply->error_code = control_take_str(root, "error_code");
    if (reply->error) {
        reply->ok = false;
        return 0;
    }
    if (op == TNY_CONTROL_OP_ASK_USER) {
        reply->ok = reply->answer != NULL;
        return 0;
    }
    yyjson_val *ok = jget(root, "ok");
    reply->ok = yyjson_is_bool(ok) && yyjson_get_bool(ok);
    return 0;
}

static tny_control_exchange control_wait_reply(int fd, tny_control_op op, const char *id,
                                               tny_control_reply *reply) {
    buf_t in;
    buf_init(&in);
    tny_control_exchange result = TNY_CONTROL_EXCHANGE_CLOSED;
    for (;;) {
        if (g_control_interrupted) {
            result = TNY_CONTROL_EXCHANGE_INTERRUPTED;
            break;
        }
        struct pollfd pf = {fd, POLLIN, 0};
        int pr = tny_poll(&pf, 1, 200);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) break;
        if (pr == 0) continue;

        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;
        buf_append(&in, chunk, (size_t)n);
        if (buf_oom(&in) || in.len > CONTROL_MAX_LINE) break;

        char *nl;
        while ((nl = memchr(in.data, '\n', in.len)) != NULL) {
            size_t len = (size_t)(nl - in.data);
            yyjson_doc *doc = jparse(in.data, len);
            buf_consume(&in, len + 1);
            if (!doc) continue;
            int taken = control_take_reply(op, id, yyjson_doc_get_root(doc), reply);
            yyjson_doc_free(doc);
            if (taken == 0) {
                buf_free(&in);
                return TNY_CONTROL_EXCHANGE_OK;
            }
        }
    }
    buf_free(&in);
    return result;
}
#endif /* !__EMSCRIPTEN__ */

tny_control_exchange tny_control_request(tny_control_op op, const char *payload,
                                         const char *expected_sha256, uint64_t expected_bytes,
                                         tny_control_reply *reply) {
    tny_control_reply local = {0};
    if (!reply) reply = &local;
    memset(reply, 0, sizeof *reply);
    const char *sock = getenv("TNY_SESSION_SOCK");
    if (!sock || !*sock) {
        if (reply == &local) tny_control_reply_free(reply);
#ifdef __EMSCRIPTEN__
        /* Preview reports runtime support independently of session discovery;
         * legacy ask-user/attach retain their missing-socket precedence. */
        if (op == TNY_CONTROL_OP_IMAGE_PREVIEW) return TNY_CONTROL_EXCHANGE_UNSUPPORTED;
#endif
        return TNY_CONTROL_EXCHANGE_NO_SOCKET;
    }
#ifdef __EMSCRIPTEN__
    (void)op;
    (void)payload;
    (void)expected_sha256;
    (void)expected_bytes;
    if (reply == &local) tny_control_reply_free(reply);
    return TNY_CONTROL_EXCHANGE_UNSUPPORTED;
#else
    char *id = gen_id();
    if (!id) {
        if (reply == &local) tny_control_reply_free(reply);
        return TNY_CONTROL_EXCHANGE_OOM;
    }
    reply->id = id;
    buf_t wire;
    buf_init(&wire);
    if (control_build_request(op, payload, expected_sha256, expected_bytes, id, &wire) != 0) {
        buf_free(&wire);
        if (reply == &local) tny_control_reply_free(reply);
        return TNY_CONTROL_EXCHANGE_OOM;
    }
    if (buf_oom(&wire)) {
        buf_free(&wire);
        if (reply == &local) tny_control_reply_free(reply);
        return TNY_CONTROL_EXCHANGE_OOM;
    }
    if (wire.len > CONTROL_MAX_LINE) {
        buf_free(&wire);
        if (reply == &local) tny_control_reply_free(reply);
        return TNY_CONTROL_EXCHANGE_TOO_LARGE;
    }

    int fd = control_connect(sock);
    if (fd < 0) {
        buf_free(&wire);
        if (reply == &local) tny_control_reply_free(reply);
        return TNY_CONTROL_EXCHANGE_CONNECT_FAILED;
    }
    struct sigaction old_int, sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = control_on_sigint;
    sigemptyset(&sa.sa_mask);
    g_control_interrupted = 0;
    sigaction(SIGINT, &sa, &old_int);

    int written = control_write_all(fd, wire.data, wire.len);
    buf_free(&wire);
    tny_control_exchange exchange;
    if (written == -2) exchange = TNY_CONTROL_EXCHANGE_INTERRUPTED;
    else if (written != 0) exchange = TNY_CONTROL_EXCHANGE_WRITE_FAILED;
    else exchange = control_wait_reply(fd, op, id, reply);
    sigaction(SIGINT, &old_int, NULL);
    close(fd);
    if (reply == &local) tny_control_reply_free(reply);
    return exchange;
#endif
}

/* ---- stdio-owning command wrappers ---- */

static void control_help_ask_user(void) {
    puts("Usage: tny ask-user [--json] QUESTION\n"
         "       printf 'QUESTION' | tny ask-user [--json]\n\n"
         "Ask the owning tny frontend a free-text question.\n\n"
         "Options: --json machine output; -h, --help show this help.");
}

static void control_help_image(void) {
    puts("Usage: tny image attach [--json] PATH\n\n"
         "Attach an image to the next provider request in this tny session.\n\n"
         "Options: --json machine output; -h, --help show this help.");
}

static int control_no_socket(void) {
    fputs("tny: no session socket (set TNY_SESSION_SOCK or run inside tny)\n", stderr);
    return 1;
}

static void control_print_one_line(const char *text) {
    fputs("tny: ", stderr);
    for (size_t i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        fputc(c == '\n' || c == '\r' ? ' ' : c, stderr);
    }
    fputc('\n', stderr);
}

static int control_print_json_field(tny_control_op op, const char *id, const char *field,
                                    const char *value) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"kind\":");
    jescape(&out, control_op_name(op));
    buf_appends(&out, ",\"id\":");
    jescape(&out, id ? id : "");
    buf_appends(&out, ",");
    jescape(&out, field);
    buf_appends(&out, ":");
    jescape(&out, value ? value : "");
    buf_appends(&out, "}\n");
    if (buf_oom(&out)) {
        buf_free(&out);
        return -1;
    }
    fwrite(out.data, 1, out.len, stdout);
    buf_free(&out);
    return ferror(stdout) ? -1 : 0;
}

/* Restore the established stdio and exit behavior of both verbs from the data
 * the primitive returned. */
static int control_report(tny_control_op op, bool json, tny_control_exchange exchange,
                          const tny_control_reply *reply) {
    switch (exchange) {
    case TNY_CONTROL_EXCHANGE_OK: break;
    case TNY_CONTROL_EXCHANGE_NO_SOCKET: return control_no_socket();
    case TNY_CONTROL_EXCHANGE_UNSUPPORTED:
        fputs("tny: session control is unavailable in WebAssembly\n", stderr);
        return 1;
    case TNY_CONTROL_EXCHANGE_OOM: fputs("tny: out of memory\n", stderr); return 1;
    case TNY_CONTROL_EXCHANGE_TOO_LARGE:
        fputs("tny: control request exceeds the 1 MiB limit\n", stderr);
        return 1;
    case TNY_CONTROL_EXCHANGE_CONNECT_FAILED:
        fputs("tny: could not connect to session socket\n", stderr);
        return 1;
    case TNY_CONTROL_EXCHANGE_WRITE_FAILED:
        fputs("tny: could not write to session socket\n", stderr);
        return 2;
    case TNY_CONTROL_EXCHANGE_INTERRUPTED: fputs("tny: interrupted\n", stderr); return 130;
    case TNY_CONTROL_EXCHANGE_CLOSED:
        fputs("tny: session control connection closed before a reply\n", stderr);
        return 2;
    }
    if (reply->error) {
        if (json) {
            if (control_print_json_field(op, reply->id, "error", reply->error) != 0)
                fputs("tny: could not write JSON output\n", stderr);
        } else {
            control_print_one_line(reply->error);
        }
        return 2;
    }
    if (!reply->ok) return 2;
    if (op == TNY_CONTROL_OP_ASK_USER) {
        if (json) {
            if (control_print_json_field(op, reply->id, "answer", reply->answer) != 0) {
                fputs("tny: could not write JSON output\n", stderr);
                return 2;
            }
        } else {
            size_t len = strlen(reply->answer);
            if (len) fwrite(reply->answer, 1, len, stdout);
            if (!len || reply->answer[len - 1] != '\n') fputc('\n', stdout);
            if (ferror(stdout)) return 2;
        }
        return 0;
    }
    if (json)
        printf("{\"kind\":\"%s\",\"id\":\"%s\",\"ok\":true}\n", control_op_name(op),
               reply->id ? reply->id : "");
    return ferror(stdout) ? 2 : 0;
}

static int control_exchange(tny_control_op op, const char *payload, bool json) {
    tny_control_reply reply = {0};
    tny_control_exchange exchange = tny_control_request(op, payload, NULL, 0, &reply);
    int rc = control_report(op, json, exchange, &reply);
    tny_control_reply_free(&reply);
    return rc;
}

static char *control_read_stdin(void) {
    buf_t input;
    buf_init(&input);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        buf_append(&input, chunk, n);
        if (buf_oom(&input) || input.len > CONTROL_MAX_LINE) break;
    }
    if (ferror(stdin) || buf_oom(&input) || input.len > CONTROL_MAX_LINE) {
        buf_free(&input);
        return NULL;
    }
    return buf_detach(&input);
}

int cmd_ask_user(bool json, int argc, char **argv) {
    bool positional = false;
    buf_t question;
    buf_init(&question);
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional && strcmp(arg, "--") == 0) {
            positional = true;
        } else if (!positional && strcmp(arg, "--json") == 0) {
            json = true;
        } else if (!positional && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            control_help_ask_user();
            buf_free(&question);
            return 0;
        } else if (!positional && arg[0] == '-') {
            fprintf(stderr, "tny: ask-user: unknown flag %s\n", arg);
            buf_free(&question);
            return 1;
        } else {
            if (question.len) buf_appends(&question, " ");
            buf_appends(&question, arg);
            positional = true;
        }
    }
    const char *sock = getenv("TNY_SESSION_SOCK");
    if (!sock || !*sock) {
        buf_free(&question);
        return control_no_socket();
    }
    char *stdin_question = NULL;
    if (!question.len) {
        stdin_question = control_read_stdin();
        if (!stdin_question) {
            fputs("tny: ask-user: could not read question from stdin\n", stderr);
            buf_free(&question);
            return 1;
        }
    }
    const char *text = question.len ? question.data : stdin_question;
    if (!text || !*text) {
        fputs("tny: ask-user: QUESTION is required (argument or stdin)\n", stderr);
        free(stdin_question);
        buf_free(&question);
        return 1;
    }
    int rc = control_exchange(TNY_CONTROL_OP_ASK_USER, text, json);
    free(stdin_question);
    buf_free(&question);
    return rc;
}

int cmd_image(bool json, int argc, char **argv) {
    const char *sub = NULL, *path = NULL;
    bool positional = false;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional && strcmp(arg, "--") == 0) {
            positional = true;
        } else if (!positional && strcmp(arg, "--json") == 0) {
            json = true;
        } else if (!positional && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            control_help_image();
            return 0;
        } else if (!positional && arg[0] == '-') {
            fprintf(stderr, "tny: image: unknown flag %s\n", arg);
            return 1;
        } else if (!sub) {
            sub = arg;
        } else if (!path) {
            path = arg;
            positional = true;
        } else {
            fputs("tny: image attach accepts exactly one PATH\n", stderr);
            return 1;
        }
    }
    if (!sub || strcmp(sub, "attach") != 0 || !path || !*path) {
        fputs("tny: image: expected `attach PATH`\n", stderr);
        return 1;
    }
    return control_exchange(TNY_CONTROL_OP_IMAGE_ATTACH, path, json);
}
