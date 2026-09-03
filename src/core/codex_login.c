/* codex_login.c — native ChatGPT sign-in for the builtin codex profile
 * (docs/adr/0066). No Codex CLI required.
 *
 * Two flows, both pinned to what the Codex CLI (codex-rs/login) and pi
 * (packages/ai/src/utils/oauth/openai-codex.ts) ship:
 *
 *   browser (default)
 *     GET  {issuer}/oauth/authorize?response_type=code&client_id&redirect_uri
 *          =http://localhost:1455/auth/callback&scope=openid profile email
 *          offline_access&code_challenge=S256(verifier)&code_challenge_method
 *          =S256&state&id_token_add_organizations=true
 *          &codex_cli_simplified_flow=true&originator=tny
 *     tny listens on 127.0.0.1:1455 (1457 fallback, like the CLI) for
 *     /auth/callback?code&state, and also accepts the redirect URL or the
 *     bare code pasted on stdin (headless boxes whose browser is elsewhere).
 *     POST {issuer}/oauth/token  grant_type=authorization_code, code,
 *          redirect_uri, client_id, code_verifier   (form encoded)
 *       -> { id_token, access_token, refresh_token, expires_in }
 *
 *   device (--device)
 *     POST {issuer}/api/accounts/deviceauth/usercode {client_id}
 *       -> { device_auth_id, user_code, interval }
 *     user opens {issuer}/codex/device and types the code;
 *     POST {issuer}/api/accounts/deviceauth/token {device_auth_id, user_code}
 *       -> 403/404 pending … 200 { authorization_code, code_verifier }
 *     then the same token exchange with redirect_uri
 *     {issuer}/deviceauth/callback and the server-issued verifier.
 *
 * Success lands in ~/.tny/codex-auth.json (codex_auth.c), which the codex
 * profile prefers over the Codex CLI's file and refreshes itself.
 * TNY_CODEX_OAUTH_ISSUER and TNY_CODEX_CALLBACK_PORT redirect the flow at
 * a mock for tests. Never print tokens (CLAUDE.md). */
#include "core/config.h"
#include "json/json.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_OAUTH_ISSUER    "https://auth.openai.com"
#define CODEX_OAUTH_SCOPE     "openid profile email offline_access"
#define CODEX_CALLBACK_PORT   1455
#define CODEX_CALLBACK_PORT2  1457 /* the CLI's fallback (Hydra allow-list) */
#define CODEX_CALLBACK_PATH   "/auth/callback"
#define CODEX_LOGIN_WAIT_MS   (10 * 60 * 1000)
#define CODEX_DEVICE_WAIT_MS  (15 * 60 * 1000)
#define CODEX_DEVICE_INTERVAL 5

static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const char *issuer(void) {
    const char *v = getenv("TNY_CODEX_OAUTH_ISSUER");
    return v && *v ? v : CODEX_OAUTH_ISSUER;
}

/* Best-effort browser launch; the URL is on stdout either way. */
static void open_browser(const char *url) {
    if (getenv("TNY_CODEX_OAUTH_ISSUER")) return; /* tests: never pop a browser */
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

static void print_oauth_error(const char *what, int status, const buf_t *body) {
    yyjson_doc *doc = jparse(body->data, body->len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *desc = jget_str(root, "error_description");
    if (!desc) desc = jget_str(root, "error");
    fprintf(stderr, "tny: %s failed (HTTP %d)%s%s\n", what, status, desc ? ": " : "",
            desc ? desc : "");
    yyjson_doc_free(doc);
}

/* ---------- token exchange + store ---------- */

static int exchange_code(const char *code, const char *verifier, const char *redirect_uri) {
    buf_t form, body, url;
    buf_init(&form);
    buf_init(&body);
    buf_init(&url);
    url_form_append(&form, "grant_type", "authorization_code");
    url_form_append(&form, "code", code);
    url_form_append(&form, "redirect_uri", redirect_uri);
    url_form_append(&form, "client_id", CODEX_OAUTH_CLIENT_ID);
    url_form_append(&form, "code_verifier", verifier);
    buf_appendf(&url, "%s/oauth/token", issuer());
    char err[256] = "";
    int status = tny_codex_http_post(url.data, "application/x-www-form-urlencoded", form.data,
                                     &body, err, sizeof err);
    secure_zero(form.data, form.len);
    buf_free(&form);
    buf_free(&url);
    int rc = 1;
    if (status < 0) {
        fprintf(stderr, "tny: cannot reach %s: %s\n", issuer(), err);
    } else if (status < 200 || status >= 300) {
        print_oauth_error("token exchange", status, &body);
    } else {
        yyjson_doc *tok = jparse(body.data, body.len);
        yyjson_val *root = tok ? yyjson_doc_get_root(tok) : NULL;
        const char *access = jget_str(root, "access_token");
        if (!access || !*access) {
            fprintf(stderr, "tny: token exchange returned no access_token\n");
        } else if (tny_codex_store_save(root) != 0) {
            fprintf(stderr, "tny: sign-in succeeded but writing ~/.tny/codex-auth.json failed\n");
        } else {
            char *p = tny_codex_store_path();
            printf("Signed in. ChatGPT login saved to %s — `tny --provider codex` uses it "
                   "and refreshes it.\n",
                   p ? p : "~/.tny/codex-auth.json");
            free(p);
            rc = 0;
        }
        yyjson_doc_free(tok);
    }
    if (body.data) secure_zero(body.data, body.len);
    buf_free(&body);
    return rc;
}

/* ---------- device-code flow ---------- */

static int login_device(void) {
    buf_t req, body, url;
    buf_init(&req);
    buf_init(&body);
    buf_init(&url);
    buf_appends(&req, "{\"client_id\":\"" CODEX_OAUTH_CLIENT_ID "\"}");
    buf_appendf(&url, "%s/api/accounts/deviceauth/usercode", issuer());
    char err[256] = "";
    int status =
        tny_codex_http_post(url.data, "application/json", req.data, &body, err, sizeof err);
    buf_free(&req);
    buf_free(&url);
    if (status < 0) {
        fprintf(stderr, "tny: cannot reach %s: %s\n", issuer(), err);
        buf_free(&body);
        return 1;
    }
    if (status == 404) {
        fprintf(stderr,
                "tny: device-code login is not enabled at %s; use the browser "
                "flow (`tny --provider codex login`).\n",
                issuer());
        buf_free(&body);
        return 1;
    }
    if (status < 200 || status >= 300) {
        print_oauth_error("device-code request", status, &body);
        buf_free(&body);
        return 1;
    }
    yyjson_doc *doc = jparse(body.data, body.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *id = jget_str(root, "device_auth_id");
    const char *uc = jget_str(root, "user_code");
    if (!uc) uc = jget_str(root, "usercode");
    /* the CLI accepts a string interval; the API sends whole seconds */
    int64_t interval = jget_int(root, "interval", -1);
    if (interval < 0) {
        const char *s = jget_str(root, "interval");
        interval = s ? atoi(s) : CODEX_DEVICE_INTERVAL;
    }
    if (interval <= 0) interval = CODEX_DEVICE_INTERVAL;
    char *device_id = id ? xstrdup(id) : NULL;
    char *user_code = uc ? xstrdup(uc) : NULL;
    yyjson_doc_free(doc);
    buf_free(&body);
    if (!device_id || !user_code) {
        free(device_id);
        free(user_code);
        fprintf(stderr, "tny: malformed device-code response from %s\n", issuer());
        return 1;
    }

    printf("Sign in with ChatGPT using a device code:\n\n"
           "1. Open this link in a browser on any device and sign in:\n\n"
           "   %s/codex/device\n\n"
           "2. Enter this one-time code (expires in 15 minutes):\n\n"
           "   %s\n\n"
           "Continue only if you started this login in tny. If a website or another "
           "person gave you this code, cancel.\n\n"
           "Waiting for authorization (Ctrl-C aborts)…\n",
           issuer(), user_code);
    fflush(stdout);

    int rc = 1;
    int64_t deadline = now_ms() + CODEX_DEVICE_WAIT_MS;
    for (;;) {
        for (int64_t slept = 0; slept < interval * 1000 && !g_interrupted; slept += 200)
            tny_poll(NULL, 0, 200);
        if (g_interrupted) {
            fprintf(stderr, "tny: login aborted\n");
            break;
        }
        if (now_ms() > deadline) {
            fprintf(stderr, "tny: device code expired; run `tny --provider codex login "
                            "--device` again.\n");
            break;
        }
        buf_t poll_req, poll_body, poll_url;
        buf_init(&poll_req);
        buf_init(&poll_body);
        buf_init(&poll_url);
        buf_appends(&poll_req, "{\"device_auth_id\":");
        jescape(&poll_req, device_id);
        buf_appends(&poll_req, ",\"user_code\":");
        jescape(&poll_req, user_code);
        buf_appends(&poll_req, "}");
        buf_appendf(&poll_url, "%s/api/accounts/deviceauth/token", issuer());
        err[0] = 0;
        status = tny_codex_http_post(poll_url.data, "application/json", poll_req.data, &poll_body,
                                     err, sizeof err);
        buf_free(&poll_req);
        buf_free(&poll_url);
        if (status < 0) {
            fprintf(stderr, "tny: cannot reach %s: %s\n", issuer(), err);
            buf_free(&poll_body);
            break;
        }
        if (status == 403 || status == 404) { /* authorization pending */
            buf_free(&poll_body);
            continue;
        }
        if (status < 200 || status >= 300) {
            yyjson_doc *edoc = jparse(poll_body.data, poll_body.len);
            yyjson_val *eroot = edoc ? yyjson_doc_get_root(edoc) : NULL;
            yyjson_val *ev = jget(eroot, "error");
            const char *code = yyjson_is_str(ev) ? yyjson_get_str(ev) : jget_str(ev, "code");
            bool pending = code && strcmp(code, "deviceauth_authorization_pending") == 0;
            bool slow = code && strcmp(code, "slow_down") == 0;
            yyjson_doc_free(edoc);
            if (slow) interval += CODEX_DEVICE_INTERVAL;
            if (pending || slow) {
                buf_free(&poll_body);
                continue;
            }
            print_oauth_error("device authorization", status, &poll_body);
            buf_free(&poll_body);
            break;
        }
        yyjson_doc *cdoc = jparse(poll_body.data, poll_body.len);
        yyjson_val *croot = cdoc ? yyjson_doc_get_root(cdoc) : NULL;
        const char *auth_code = jget_str(croot, "authorization_code");
        const char *verifier = jget_str(croot, "code_verifier");
        if (auth_code && verifier) {
            buf_t redirect;
            buf_init(&redirect);
            buf_appendf(&redirect, "%s/deviceauth/callback", issuer());
            rc = exchange_code(auth_code, verifier, redirect.data);
            buf_free(&redirect);
        } else {
            fprintf(stderr, "tny: malformed device authorization response\n");
        }
        yyjson_doc_free(cdoc);
        if (poll_body.data) secure_zero(poll_body.data, poll_body.len);
        buf_free(&poll_body);
        break;
    }
    free(device_id);
    free(user_code);
    return rc;
}

/* ---------- browser (PKCE) flow ---------- */

/* Percent-decode into a malloc'd string ('+' stays: OAuth codes never
 * carry it, and query values are percent-encoded). */
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

/* Value of `key` in a query string (`a=1&b=2`), malloc'd, or NULL. */
static char *query_get(const char *query, size_t qlen, const char *key) {
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

/* Pull code/state out of what the user pasted: the whole redirect URL, a
 * `code=…&state=…` query, `code#state`, or the bare code. */
static void parse_pasted(const char *line, char **code, char **state) {
    *code = *state = NULL;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
    if (!n) return;
    const char *q = memchr(line, '?', n);
    if (q) {
        q++;
        *code = query_get(q, (size_t)(line + n - q), "code");
        *state = query_get(q, (size_t)(line + n - q), "state");
        return;
    }
    if (memchr(line, '=', n)) {
        *code = query_get(line, n, "code");
        *state = query_get(line, n, "state");
        return;
    }
    const char *hash = memchr(line, '#', n);
    if (hash) {
        *code = xstrndup(line, (size_t)(hash - line));
        *state = xstrndup(hash + 1, (size_t)(line + n - hash - 1));
        return;
    }
    *code = xstrndup(line, n);
}

#ifndef __EMSCRIPTEN__
static int listen_loopback(int port) {
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

static void http_reply(int fd, int status, const char *html) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
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

/* Serve one connection: a GET /auth/callback?code&state with the expected
 * state hands back the code (malloc'd); anything else is answered and
 * ignored. The browser gets a tiny page either way. */
static char *serve_callback(int lfd, const char *expect_state) {
    if (lfd < 0) return NULL;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return NULL;
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
    char *code = NULL;
    const char *path = str_starts(req, "GET ") ? req + 4 : NULL;
    const char *sp = path ? strchr(path, ' ') : NULL;
    if (path && sp && str_starts(path, CODEX_CALLBACK_PATH) &&
        (path[strlen(CODEX_CALLBACK_PATH)] == '?' || path + strlen(CODEX_CALLBACK_PATH) == sp)) {
        const char *q = memchr(path, '?', (size_t)(sp - path));
        size_t qlen = q ? (size_t)(sp - q - 1) : 0;
        char *state = q ? query_get(q + 1, qlen, "state") : NULL;
        char *err = q ? query_get(q + 1, qlen, "error") : NULL;
        code = q ? query_get(q + 1, qlen, "code") : NULL;
        if (!state || strcmp(state, expect_state) != 0) {
            http_reply(fd, 400, "<h1>State mismatch</h1><p>Start the login again in tny.</p>");
            free(code);
            code = NULL;
        } else if (err) {
            fprintf(stderr, "tny: sign-in refused: %s\n", err);
            http_reply(fd, 400, "<h1>Sign-in refused</h1><p>See the terminal.</p>");
            free(code);
            code = NULL;
        } else if (!code || !*code) {
            http_reply(fd, 400, "<h1>Missing authorization code</h1>");
            free(code);
            code = NULL;
        } else {
            http_reply(fd, 200,
                       "<h1>Signed in to tny</h1><p>You can close this tab and return to "
                       "the terminal.</p>");
        }
        free(state);
        free(err);
    } else {
        http_reply(fd, 404, "<h1>Not found</h1>");
    }
    close(fd);
    return code;
}
#endif

static int login_browser(void) {
    uint8_t rnd[32];
    if (!random_bytes(rnd, sizeof rnd)) {
        fprintf(stderr, "tny: no CSPRNG available (/dev/urandom); cannot start a login\n");
        return 1;
    }
    buf_t verifier, state, challenge;
    buf_init(&verifier);
    buf_init(&state);
    buf_init(&challenge);
    b64url_encode(rnd, sizeof rnd, &verifier); /* 43 chars: RFC 7636 §4.1 */
    if (!random_bytes(rnd, sizeof rnd)) {
        buf_free(&verifier);
        buf_free(&state);
        buf_free(&challenge);
        return 1;
    }
    b64url_encode(rnd, sizeof rnd, &state);
    uint8_t digest[32];
    sha256((const uint8_t *)verifier.data, verifier.len, digest);
    b64url_encode(digest, sizeof digest, &challenge);

    int port = CODEX_CALLBACK_PORT;
    const char *pe = getenv("TNY_CODEX_CALLBACK_PORT");
    if (pe && atoi(pe) > 0) port = atoi(pe);
    int lfd = -1;
#ifndef __EMSCRIPTEN__
    lfd = listen_loopback(port);
    if (lfd < 0 && !pe) {
        port = CODEX_CALLBACK_PORT2;
        lfd = listen_loopback(port);
    }
#endif
    buf_t redirect, url, form;
    buf_init(&redirect);
    buf_init(&url);
    buf_init(&form);
    buf_appendf(&redirect, "http://localhost:%d" CODEX_CALLBACK_PATH, port);
    url_form_append(&form, "response_type", "code");
    url_form_append(&form, "client_id", CODEX_OAUTH_CLIENT_ID);
    url_form_append(&form, "redirect_uri", redirect.data);
    url_form_append(&form, "scope", CODEX_OAUTH_SCOPE);
    url_form_append(&form, "code_challenge", challenge.data);
    url_form_append(&form, "code_challenge_method", "S256");
    url_form_append(&form, "state", state.data);
    url_form_append(&form, "id_token_add_organizations", "true");
    url_form_append(&form, "codex_cli_simplified_flow", "true");
    url_form_append(&form, "originator", "tny");
    buf_appendf(&url, "%s/oauth/authorize?%s", issuer(), form.data);
    buf_free(&form);

    printf("Sign in with your ChatGPT account:\n\n  %s\n\n", url.data);
    if (lfd >= 0)
        printf("Waiting for the browser to return to %s (Ctrl-C aborts).\n", redirect.data);
    else
        printf("Could not listen on %s (port busy, or no sockets on this build).\n", redirect.data);
    bool tty = isatty(0);
    if (tty)
        printf("If the browser is on another machine, paste the redirect URL (or the "
               "authorization code) here and press Enter.\n");
    fflush(stdout);
    open_browser(url.data);
    buf_free(&url);

    char *code = NULL;
    int64_t deadline = now_ms() + CODEX_LOGIN_WAIT_MS;
    while (!code && !g_interrupted) {
        if (now_ms() > deadline) {
            fprintf(stderr, "tny: login timed out after %d minutes\n", CODEX_LOGIN_WAIT_MS / 60000);
            break;
        }
        if (lfd < 0 && !tty) {
            fprintf(stderr, "tny: no callback listener and no terminal to paste into; use "
                            "`tny --provider codex login --device`\n");
            break;
        }
        struct pollfd pf[2];
        int n = 0;
        if (lfd >= 0) pf[n++] = (struct pollfd){lfd, POLLIN, 0};
        if (tty) pf[n++] = (struct pollfd){0, POLLIN, 0};
        if (tny_poll(pf, n, 500) <= 0) continue;
        for (int i = 0; i < n && !code; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP))) continue;
#ifndef __EMSCRIPTEN__
            if (pf[i].fd == lfd) {
                code = serve_callback(lfd, state.data);
                continue;
            }
#endif
            char line[4096];
            if (!fgets(line, sizeof line, stdin)) {
                tty = false; /* stdin closed: keep waiting on the listener */
                continue;
            }
            char *pc = NULL, *ps = NULL;
            parse_pasted(line, &pc, &ps);
            if (ps && strcmp(ps, state.data) != 0) {
                fprintf(stderr, "tny: state mismatch in the pasted URL; paste the URL from "
                                "THIS login\n");
                free(pc);
                pc = NULL;
            }
            free(ps);
            if (pc && *pc) code = pc;
            else free(pc);
        }
    }
    if (lfd >= 0) close(lfd);
    int rc = 1;
    if (g_interrupted) fprintf(stderr, "tny: login aborted\n");
    else if (code) rc = exchange_code(code, verifier.data, redirect.data);
    if (code) secure_free(code);
    secure_zero(verifier.data, verifier.len);
    buf_free(&verifier);
    buf_free(&state);
    buf_free(&challenge);
    buf_free(&redirect);
    return rc;
}

int tny_codex_login(tny_ctx *ctx, bool device) {
    (void)ctx;
    struct sigaction sa = {0}, old_sa;
    sa.sa_handler = on_sigint;
    g_interrupted = 0;
    sigaction(SIGINT, &sa, &old_sa);
    int rc = device ? login_device() : login_browser();
    sigaction(SIGINT, &old_sa, NULL);
    if (g_interrupted) return 130;
    return rc;
}
