/* cmd_control.c — tool-role clients for the session control channel.
 *
 * These verbs deliberately create no tny_ctx and never consult /dev/tty.
 * A subprocess launched by `terminal` receives the runner's resolved socket
 * in TNY_SESSION_SOCK, handshakes as role `tool`, then performs one correlated
 * NDJSON request (ADR 0057, ADR 0058). */
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

typedef enum { CONTROL_ASK_USER, CONTROL_IMAGE_ATTACH } control_kind;

static volatile sig_atomic_t g_control_interrupted;

static void control_on_sigint(int sig) {
    (void)sig;
    g_control_interrupted = 1;
}

static const char *control_kind_name(control_kind kind) {
    return kind == CONTROL_ASK_USER ? "ask_user" : "image_attach";
}

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
        ssize_t n = write(fd, data + off, len - off);
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

static void control_print_one_line(const char *text, size_t len) {
    fputs("tny: ", stderr);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        fputc(c == '\n' || c == '\r' ? ' ' : c, stderr);
    }
    fputc('\n', stderr);
}

static int control_print_json_value(control_kind kind, const char *id, const char *field,
                                    yyjson_val *value) {
    char *encoded = jwrite_val(value);
    if (!encoded) return -1;
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"kind\":");
    jescape(&out, control_kind_name(kind));
    buf_appends(&out, ",\"id\":");
    jescape(&out, id);
    buf_appends(&out, ",");
    jescape(&out, field);
    buf_appends(&out, ":");
    buf_appends(&out, encoded);
    buf_appends(&out, "}\n");
    free(encoded);
    if (buf_oom(&out)) {
        buf_free(&out);
        return -1;
    }
    fwrite(out.data, 1, out.len, stdout);
    buf_free(&out);
    return ferror(stdout) ? -1 : 0;
}

/* Returns -1 for an unrelated message, otherwise the command exit code. */
static int control_handle_reply(control_kind kind, const char *id, bool json, yyjson_val *root) {
    yyjson_val *rid = jget(root, "id");
    if (!yyjson_is_str(rid) || strcmp(yyjson_get_str(rid), id) != 0) return -1;

    yyjson_val *error = jget(root, "error");
    if (yyjson_is_str(error)) {
        if (json) {
            if (control_print_json_value(kind, id, "error", error) != 0)
                fputs("tny: could not write JSON output\n", stderr);
        } else {
            control_print_one_line(yyjson_get_str(error), yyjson_get_len(error));
        }
        return 2;
    }
    if (kind == CONTROL_ASK_USER) {
        yyjson_val *answer = jget(root, "answer");
        if (!yyjson_is_str(answer)) return 2;
        if (json) {
            if (control_print_json_value(kind, id, "answer", answer) != 0) {
                fputs("tny: could not write JSON output\n", stderr);
                return 2;
            }
        } else {
            const char *text = yyjson_get_str(answer);
            size_t len = yyjson_get_len(answer);
            if (len) fwrite(text, 1, len, stdout);
            if (!len || text[len - 1] != '\n') fputc('\n', stdout);
            if (ferror(stdout)) return 2;
        }
        return 0;
    }

    yyjson_val *ok = jget(root, "ok");
    if (!yyjson_is_bool(ok) || !yyjson_get_bool(ok)) return 2;
    if (json) printf("{\"kind\":\"image_attach\",\"id\":\"%s\",\"ok\":true}\n", id);
    return ferror(stdout) ? 2 : 0;
}

static int control_wait_reply(int fd, control_kind kind, const char *id, bool json) {
    buf_t in;
    buf_init(&in);
    int result = 2;
    for (;;) {
        if (g_control_interrupted) {
            result = 130;
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
            int handled = control_handle_reply(kind, id, json, yyjson_doc_get_root(doc));
            yyjson_doc_free(doc);
            if (handled >= 0) {
                buf_free(&in);
                return handled;
            }
        }
    }
    buf_free(&in);
    if (result == 130) fputs("tny: interrupted\n", stderr);
    else fputs("tny: session control connection closed before a reply\n", stderr);
    return result;
}

static int control_exchange(control_kind kind, const char *payload, bool json) {
    const char *sock = getenv("TNY_SESSION_SOCK");
    if (!sock || !*sock) return control_no_socket();
#ifdef __EMSCRIPTEN__
    (void)kind;
    (void)payload;
    (void)json;
    fputs("tny: session control is unavailable in WebAssembly\n", stderr);
    return 1;
#else
    char *id = gen_id();
    if (!id) {
        fputs("tny: out of memory\n", stderr);
        return 1;
    }
    buf_t wire;
    buf_init(&wire);
    buf_appends(&wire, "{\"op\":\"hello\",\"role\":\"tool\"}\n{\"op\":");
    jescape(&wire, control_kind_name(kind));
    buf_appends(&wire, ",\"id\":");
    jescape(&wire, id);
    buf_appends(&wire, kind == CONTROL_ASK_USER ? ",\"question\":" : ",\"path\":");
    jescape(&wire, payload);
    buf_appends(&wire, "}\n");
    if (buf_oom(&wire) || wire.len > CONTROL_MAX_LINE) {
        fputs("tny: control request exceeds the 1 MiB limit\n", stderr);
        buf_free(&wire);
        free(id);
        return 1;
    }

    int fd = control_connect(sock);
    if (fd < 0) {
        fputs("tny: could not connect to session socket\n", stderr);
        buf_free(&wire);
        free(id);
        return 1;
    }
    struct sigaction old_int, sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = control_on_sigint;
    sigemptyset(&sa.sa_mask);
    g_control_interrupted = 0;
    sigaction(SIGINT, &sa, &old_int);

    int written = control_write_all(fd, wire.data, wire.len);
    buf_free(&wire);
    int rc;
    if (written == -2) {
        fputs("tny: interrupted\n", stderr);
        rc = 130;
    } else if (written != 0) {
        fputs("tny: could not write to session socket\n", stderr);
        rc = 2;
    } else {
        rc = control_wait_reply(fd, kind, id, json);
    }
    sigaction(SIGINT, &old_int, NULL);
    close(fd);
    free(id);
    return rc;
#endif
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
    int rc = control_exchange(CONTROL_ASK_USER, text, json);
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
    return control_exchange(CONTROL_IMAGE_ATTACH, path, json);
}
