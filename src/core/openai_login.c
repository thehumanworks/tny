/* openai_login.c — Sign in with ChatGPT for the builtin openai provider
 * (docs/adr/0186, docs/backends/openai-chatgpt.md). No Codex CLI, no API
 * key, no client secret.
 *
 * Authorization Code + PKCE with dynamic client registration, pinned to the
 * SIWC open-source flow (developers.openai.com/siwc) and pi's
 * openai-chatgpt.ts:
 *
 *   GET {issuer}/api/accounts/authorize?response_type=code
 *       &client_id=dynamic_agent_client   (first sign-in on this host)
 *                 | <issued oaiapp_… id>    (later sign-ins)
 *       &agent_name_hint=tny               (first sign-in only)
 *       &ext_agent_host_id=urn:uuid:…      (stable per host)
 *       &redirect_uri=http://127.0.0.1:1455/auth/callback
 *       &scope=openid profile email offline_access resource.invoke
 *              chatgpt.tokens.use.direct
 *       &resource=https://api.openai.com/v1
 *       &state&nonce&code_challenge=S256(verifier)&code_challenge_method=S256
 *       [&login_hint=<saved email>] [&prompt=consent after a declined grant]
 *   -> 127.0.0.1:<port>/auth/callback?code&state[&client_id=oaiapp_…]
 *   POST {issuer}/api/accounts/oauth/token   (form) grant_type=
 *        authorization_code, the issued client_id, code, code_verifier,
 *        redirect_uri, resource
 *   -> { access_token, refresh_token, id_token, scope, expires_in, … }
 *
 * The redirect is 127.0.0.1, never `localhost`; only the port may vary, so
 * a busy 1455 falls back to an OS-assigned port. A browser on another
 * machine cannot reach the listener: paste its full redirect URL instead.
 * `id_token_hint` is deliberately not sent, because tny prints the authorize
 * URL and a token must never reach the terminal; the saved email rides
 * `login_hint`. There is no device-code flow for this client.
 * TNY_OPENAI_OAUTH_ISSUER and TNY_OPENAI_CALLBACK_PORT redirect the flow at
 * a loopback mock for tests. Never print tokens (CLAUDE.md). */
#include "core/oauth_loopback.h"
#include "core/openai_auth.h"
#include "json/json.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPENAI_REGISTER_CLIENT_ID "dynamic_agent_client"
#define OPENAI_AGENT_NAME         "tny"
#define OPENAI_OAUTH_SCOPE \
    "openid profile email offline_access resource.invoke " TNY_OPENAI_SIGNIN_SCOPE
#define OPENAI_CALLBACK_PORT 1455
#define OPENAI_CALLBACK_PATH "/auth/callback"
#define OPENAI_LOGIN_WAIT_MS (10 * 60 * 1000)

static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void print_oauth_error(const char *what, int status, const buf_t *body) {
    yyjson_doc *doc = jparse(body->data, body->len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *e = jget(root, "error");
    const char *code = yyjson_is_str(e) ? yyjson_get_str(e) : jget_str(e, "code");
    const char *desc = jget_str(root, "error_description");
    if (!desc) desc = jget_str(e, "message");
    fprintf(stderr, "tny: %s failed (HTTP %d)%s%s%s%s\n", what, status, code ? ": " : "",
            code ? code : "", desc ? " — " : "", desc ? desc : "");
    if (code && strcmp(code, "invalid_grant") == 0)
        fprintf(stderr, "tny: that authorization code is spent; run the login again\n");
    yyjson_doc_free(doc);
}

/* One sign-in attempt's secrets and the registration it continues. */
typedef struct {
    char *saved_client; /* issued id from an earlier sign-in, or NULL */
    char *saved_subject;
    char *saved_email;
    bool consent; /* plan usage was declined last time: ask again */
    buf_t verifier, state, nonce, redirect;
} attempt;

static void attempt_free(attempt *a) {
    free(a->saved_client);
    free(a->saved_subject);
    free(a->saved_email);
    if (a->verifier.data) secure_zero(a->verifier.data, a->verifier.len);
    buf_free(&a->verifier);
    buf_free(&a->state);
    buf_free(&a->nonce);
    buf_free(&a->redirect);
}

static bool random_b64url(buf_t *out) {
    uint8_t rnd[32];
    if (!random_bytes(rnd, sizeof rnd)) return false;
    b64url_encode(rnd, sizeof rnd, out); /* 43 chars: RFC 7636 §4.1 */
    secure_zero(rnd, sizeof rnd);
    return !buf_oom(out);
}

/* ---------- token exchange ---------- */

static int exchange_code(attempt *a, const char *code, const char *client_id) {
    buf_t form, body, url;
    buf_init(&form);
    buf_init(&body);
    buf_init(&url);
    url_form_append(&form, "grant_type", "authorization_code");
    url_form_append(&form, "client_id", client_id);
    url_form_append(&form, "code", code);
    url_form_append(&form, "code_verifier", a->verifier.data);
    url_form_append(&form, "redirect_uri", a->redirect.data);
    url_form_append(&form, "resource", TNY_OPENAI_API_BASE_URL);
    buf_appendf(&url, "%s/api/accounts/oauth/token", tny_openai_issuer());
    char err[256] = "";
    int status = tny_codex_http_post(url.data, "application/x-www-form-urlencoded", form.data,
                                     &body, err, sizeof err);
    secure_zero(form.data, form.len);
    buf_free(&form);
    buf_free(&url);
    int rc = 1;
    yyjson_doc *tok = NULL;
    char *sub = NULL, *email = NULL;
    if (status < 0) {
        fprintf(stderr, "tny: cannot reach %s: %s\n", tny_openai_issuer(), err);
        goto done;
    }
    if (status < 200 || status >= 300) {
        print_oauth_error("token exchange", status, &body);
        goto done;
    }
    tok = jparse(body.data, body.len);
    yyjson_val *root = tok ? yyjson_doc_get_root(tok) : NULL;
    const char *access = jget_str(root, "access_token");
    const char *refresh = jget_str(root, "refresh_token");
    const char *scope = jget_str(root, "scope");
    if (!access || !*access || !refresh || !*refresh || !scope) {
        fprintf(stderr, "tny: the token response is missing access_token, refresh_token or "
                        "scope\n");
        goto done;
    }
    const char *why = NULL;
    if (tny_openai_id_token_check(jget_str(root, "id_token"), tny_openai_issuer(), client_id,
                                  a->nonce.data, &sub, &email, &why) != 0) {
        fprintf(stderr, "tny: sign-in rejected: %s\n", why);
        goto done;
    }
    if (a->saved_subject && strcmp(a->saved_subject, sub) != 0) {
        /* reauthorization must not hand this registration to another account */
        fprintf(stderr,
                "tny: that is a different ChatGPT account than the saved sign-in%s%s; run "
                "`tny --provider openai logout --forget` first to switch accounts\n",
                a->saved_email ? " for " : "", a->saved_email ? a->saved_email : "");
        goto done;
    }
    if (tny_openai_store_save_login(root, client_id, sub, email) != 0) {
        fprintf(stderr, "tny: signed in, but writing ~/.tny/openai-auth.json failed\n");
        goto done;
    }
    tny_openai_signin s;
    int usable = tny_openai_signin_read(&s);
    tny_openai_signin_free(&s);
    const char *who = email ? email : "your ChatGPT account";
    if (usable == 0) {
        printf("Signed in with ChatGPT as %s.\n\n"
               "You're using your ChatGPT plan with tny. `tny --provider openai` now sends "
               "requests on it.\nManage usage: " TNY_CHATGPT_USAGE_URL "\n",
               who);
        const char *key = getenv("OPENAI_API_KEY");
        if (key && *key)
            printf("\nOPENAI_API_KEY is set and takes precedence; unset it to use the "
                   "ChatGPT plan.\n");
        rc = 0;
    } else {
        printf("Signed in with ChatGPT as %s, but ChatGPT plan use was not granted.\n"
               "tny will not send requests on your plan. Run `tny --provider openai login` "
               "again to allow it,\nor set OPENAI_API_KEY to use API billing instead.\n",
               who);
    }
done:
    free(sub);
    free(email);
    yyjson_doc_free(tok);
    if (body.data) secure_zero(body.data, body.len);
    buf_free(&body);
    return rc;
}

/* ---------- browser flow ---------- */

static void callback_expect(const attempt *a, oauth_callback_expect *e) {
    e->path = OPENAI_CALLBACK_PATH;
    e->state = a->state.data;
    e->require_client_id = a->saved_client == NULL; /* registration */
    e->client_id = a->saved_client;
}

/* Turn a callback result into the issued client id to exchange with, or
 * NULL after printing why this attempt failed. */
static const char *callback_client(const attempt *a, const oauth_callback *cb) {
    const char *id = a->saved_client ? a->saved_client : cb->client_id;
    if (!id || strcmp(id, OPENAI_REGISTER_CLIENT_ID) == 0) {
        fprintf(stderr, "tny: the sign-in callback carried no issued client id; registration "
                        "is incomplete — run the login again\n");
        return NULL;
    }
    return id;
}

/* A pasted line must be this attempt's full redirect URL. */
static oauth_callback_kind parse_pasted(const attempt *a, char *line, oauth_callback *cb) {
    memset(cb, 0, sizeof *cb);
    str_trim(line);
    size_t rl = a->redirect.len;
    if (!*line) return OAUTH_CALLBACK_NONE;
    if (strncmp(line, a->redirect.data, rl) != 0 || line[rl] != '?') {
        fprintf(stderr, "tny: paste the full redirect URL (it starts with %s?)\n",
                a->redirect.data);
        return OAUTH_CALLBACK_NONE;
    }
    oauth_callback_expect e;
    callback_expect(a, &e);
    const char *why = NULL;
    const char *q = line + rl + 1;
    oauth_callback_kind kind = oauth_callback_parse(q, strlen(q), &e, cb, &why);
    if (kind == OAUTH_CALLBACK_NONE && why)
        fprintf(stderr, "tny: %s in the pasted URL; paste the URL from THIS login\n", why);
    return kind;
}

static int login_browser(void) {
    attempt a = {0};
    buf_init(&a.verifier);
    buf_init(&a.state);
    buf_init(&a.nonce);
    buf_init(&a.redirect);
    int rc = 1;
    int lfd = -1;
    char *host = NULL;
    buf_t url, form, challenge;
    buf_init(&url);
    buf_init(&form);
    buf_init(&challenge);
    oauth_callback cb = {0};

    tny_openai_signin saved;
    int prior = tny_openai_signin_read(&saved);
    if (prior == -3) goto done;
    a.saved_client = saved.client_id;
    a.saved_subject = saved.subject;
    a.saved_email = saved.email;
    a.consent = prior == -2;
    saved.client_id = saved.subject = saved.email = NULL;
    tny_openai_signin_free(&saved);

    host = tny_openai_host_id();
    if (!host) goto done;
    if (!random_b64url(&a.verifier) || !random_b64url(&a.state) || !random_b64url(&a.nonce)) {
        fprintf(stderr, "tny: no CSPRNG available (/dev/urandom); cannot start a login\n");
        goto done;
    }
    uint8_t digest[32];
    sha256((const uint8_t *)a.verifier.data, a.verifier.len, digest);
    b64url_encode(digest, sizeof digest, &challenge);

    int port = OPENAI_CALLBACK_PORT;
    const char *pe = getenv("TNY_OPENAI_CALLBACK_PORT");
    if (pe && atoi(pe) > 0) port = atoi(pe);
    lfd = oauth_loopback_listen(port);
    if (lfd < 0 && !pe) { /* only the port may vary: take any free one */
        lfd = oauth_loopback_listen(0);
        if (lfd >= 0) port = oauth_loopback_port(lfd);
    }
    buf_appendf(&a.redirect, "http://127.0.0.1:%d" OPENAI_CALLBACK_PATH, port);

    url_form_append(&form, "response_type", "code");
    url_form_append(&form, "client_id",
                    a.saved_client ? a.saved_client : OPENAI_REGISTER_CLIENT_ID);
    if (!a.saved_client) url_form_append(&form, "agent_name_hint", OPENAI_AGENT_NAME);
    url_form_append(&form, "ext_agent_host_id", host);
    url_form_append(&form, "redirect_uri", a.redirect.data);
    url_form_append(&form, "scope", OPENAI_OAUTH_SCOPE);
    url_form_append(&form, "resource", TNY_OPENAI_API_BASE_URL);
    url_form_append(&form, "state", a.state.data);
    url_form_append(&form, "nonce", a.nonce.data);
    url_form_append(&form, "code_challenge", challenge.data);
    url_form_append(&form, "code_challenge_method", "S256");
    if (a.saved_client && a.saved_email) url_form_append(&form, "login_hint", a.saved_email);
    if (a.saved_client && a.consent) url_form_append(&form, "prompt", "consent");
    buf_appendf(&url, "%s/api/accounts/authorize?%s", tny_openai_issuer(), form.data);
    if (buf_oom(&url) || buf_oom(&a.redirect)) goto done;

    printf("Continue with ChatGPT — open this link to sign in:\n\n  %s\n\n", url.data);
    if (lfd >= 0)
        printf("Waiting for the browser to return to %s (Ctrl-C aborts).\n", a.redirect.data);
    else
        printf("Could not listen on %s (port busy, or no sockets on this build).\n",
               a.redirect.data);
    bool tty = isatty(0);
    if (tty)
        printf("If the browser is on another machine, paste the full redirect URL from its "
               "address bar here and press Enter.\n");
    fflush(stdout);
    if (!getenv("TNY_OPENAI_OAUTH_ISSUER")) oauth_open_browser(url.data); /* tests: no browser */

    oauth_callback_kind kind = OAUTH_CALLBACK_NONE;
    int64_t deadline = now_ms() + OPENAI_LOGIN_WAIT_MS;
    while (kind == OAUTH_CALLBACK_NONE && !g_interrupted) {
        if (now_ms() > deadline) {
            fprintf(stderr, "tny: login timed out after %d minutes\n",
                    OPENAI_LOGIN_WAIT_MS / 60000);
            break;
        }
        if (lfd < 0 && !tty) {
            fprintf(stderr,
                    "tny: no callback listener and no terminal to paste the redirect "
                    "URL into; free port %d or run the login in a terminal\n",
                    port);
            break;
        }
        struct pollfd pf[2];
        int n = 0;
        if (lfd >= 0) pf[n++] = (struct pollfd){lfd, POLLIN, 0};
        if (tty) pf[n++] = (struct pollfd){0, POLLIN, 0};
        if (tny_poll(pf, n, 500) <= 0) continue;
        for (int i = 0; i < n && kind == OAUTH_CALLBACK_NONE; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP))) continue;
            if (pf[i].fd == lfd) {
                oauth_callback_expect e;
                callback_expect(&a, &e);
                kind = oauth_loopback_serve(lfd, &e, &cb);
                continue;
            }
            char line[8192];
            if (!fgets(line, sizeof line, stdin)) {
                tty = false; /* stdin closed: keep waiting on the listener */
                continue;
            }
            kind = parse_pasted(&a, line, &cb);
            secure_zero(line, sizeof line);
        }
    }
    if (lfd >= 0) close(lfd);
    lfd = -1;
    if (g_interrupted) {
        fprintf(stderr, "tny: login aborted\n");
    } else if (kind == OAUTH_CALLBACK_ERROR) {
        /* the provider (or the user) refused this attempt: stop, exchange nothing */
        if (cb.error && strcmp(cb.error, "access_denied") == 0)
            fprintf(stderr, "tny: sign-in was cancelled in the browser (access_denied)\n");
        else fprintf(stderr, "tny: sign-in refused: %s\n", cb.error ? cb.error : "unknown");
    } else if (kind == OAUTH_CALLBACK_CODE) {
        const char *client = callback_client(&a, &cb);
        if (client) rc = exchange_code(&a, cb.code, client);
    }
done:
    if (lfd >= 0) close(lfd);
    oauth_callback_free(&cb);
    free(host);
    buf_free(&url);
    buf_free(&form);
    buf_free(&challenge);
    attempt_free(&a);
    return rc;
}

int tny_openai_login(tny_ctx *ctx, bool device) {
    (void)ctx;
    if (device) {
        fprintf(stderr,
                "tny: Sign in with ChatGPT has no device-code flow. Run `tny --provider openai "
                "login` and paste the redirect URL when the browser is on another machine.\n");
        return 2;
    }
    if (!tny_openai_issuer()) {
        fprintf(stderr, "tny: TNY_OPENAI_OAUTH_ISSUER must be an HTTP(S) numeric loopback URL "
                        "(127.0.0.1)\n");
        return 2;
    }
    struct sigaction sa = {0}, old_sa;
    sa.sa_handler = on_sigint;
    g_interrupted = 0;
    sigaction(SIGINT, &sa, &old_sa);
    int rc = login_browser();
    sigaction(SIGINT, &old_sa, NULL);
    if (g_interrupted) return 130;
    return rc;
}
