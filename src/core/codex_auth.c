/* codex_auth.c — ChatGPT subscription credentials for the builtin codex
 * profile (docs/adr/0065, docs/adr/0066, docs/backends/codex.md).
 *
 * Four sources, first hit wins:
 *
 *   1. `--chatgpt-token` (+ `--chatgpt-account-id`)   ctx fields
 *   2. `CHATGPT_ACCESS_TOKEN` (+ `CHATGPT_ACCOUNT_ID`) environment
 *   3. ~/.tny/codex-auth.json     tny's own login (codex_login.c)
 *   4. $CODEX_HOME/auth.json      the Codex CLI's login (`codex login`)
 *
 * 1 and 2 need no filesystem at all — the credential for containers, CI,
 * and the browser wasm build. 3 and 4 share the Codex CLI's file shape:
 *
 *   { "auth_mode": "chatgpt",
 *     "OPENAI_API_KEY": null,
 *     "tokens": { "id_token": "<jwt>", "access_token": "<jwt>",
 *                 "refresh_token": "…", "account_id": "…" },
 *     "last_refresh": "2026-01-01T00:00:00Z",
 *     "expires_at":   "2026-01-01T01:00:00Z" }       (tny's store only)
 *
 * The access token is the bearer for chatgpt.com/backend-api/codex; the
 * ChatGPT account id (explicit, else the `https://api.openai.com/auth`
 * .chatgpt_account_id JWT claim of the access token, then the id token)
 * rides the `chatgpt-account-id` header. A Codex CLI file holding
 * `OPENAI_API_KEY` (auth_mode "apikey") means the plain OpenAI API.
 *
 * Refresh mirrors the Codex CLI and pi: POST auth.openai.com/oauth/token
 * with the shared public client id and grant_type refresh_token when the
 * access token's `exp` (or the store's expires_at) is at/near now, or
 * last_refresh is older than eight days; the rotated tokens are written
 * back into the file they came from, so tny and the Codex CLI keep sharing
 * a CLI session while tny's own store stays self-sufficient. Never log
 * tokens (CLAUDE.md). */
#include "core/config.h"
#include "json/json.h"
#include "net/net.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_OAUTH_ISSUER    "https://auth.openai.com"
/* Same override the Codex CLI honors (its own test suite uses it). */
#define CODEX_REFRESH_URL_ENV "CODEX_REFRESH_TOKEN_URL_OVERRIDE"
#define CODEX_AUTH_CLAIM      "https://api.openai.com/auth"
#define CODEX_REFRESH_AFTER_S (8 * 24 * 3600) /* last_refresh age, like the CLI */
#define CODEX_REFRESH_EARLY_S 60              /* before expiry, never mid-turn */
#define CODEX_HTTP_TIMEOUT_MS 30000

/* ---------- paths ---------- */

char *tny_codex_home(void) {
    const char *ch = getenv("CODEX_HOME");
    if (ch && *ch) return xstrdup(ch);
    char *home = path_home();
    if (!home) return NULL;
    char *dir = path_join(home, ".codex");
    free(home);
    return dir;
}

char *tny_codex_auth_path(void) {
    char *dir = tny_codex_home();
    if (!dir) return NULL;
    char *p = path_join(dir, "auth.json");
    free(dir);
    return p;
}

char *tny_codex_store_path(void) {
    char *dir = path_tny_dir();
    if (!dir) return NULL;
    char *p = path_join(dir, "codex-auth.json");
    free(dir);
    return p;
}

static bool path_present(char *p) {
    if (!p) return false;
    bool ok = file_exists(p);
    free(p);
    return ok;
}

static const char *env_token(void) {
    const char *t = getenv("CHATGPT_ACCESS_TOKEN");
    return t && *t ? t : NULL;
}

bool tny_codex_auth_present(void) {
    return env_token() || path_present(tny_codex_store_path()) ||
           path_present(tny_codex_auth_path());
}

const char *tny_codex_cred_source_name(tny_codex_cred_source s) {
    switch (s) {
    case TNY_CODEX_CRED_FLAG: return "--chatgpt-token";
    case TNY_CODEX_CRED_ENV: return "CHATGPT_ACCESS_TOKEN";
    case TNY_CODEX_CRED_TNY_STORE: return "~/.tny/codex-auth.json";
    case TNY_CODEX_CRED_CODEX_CLI: return "$CODEX_HOME/auth.json";
    default: return "none";
    }
}

/* ---------- JWT claims ---------- */

/* The ChatGPT account id lives in the `https://api.openai.com/auth` claim of
 * both the access and the id token. */
static char *jwt_account_id(const char *jwt) {
    yyjson_doc *doc = jwt_payload_doc(jwt);
    if (!doc) return NULL;
    const char *id =
        jget_str(jget(yyjson_doc_get_root(doc), CODEX_AUTH_CLAIM), "chatgpt_account_id");
    char *out = id && *id ? xstrdup(id) : NULL;
    yyjson_doc_free(doc);
    return out;
}

/* `exp` claim of a JWT, or -1 when absent/unparseable. */
static int64_t jwt_expiry(const char *jwt) {
    yyjson_doc *doc = jwt_payload_doc(jwt);
    if (!doc) return -1;
    int64_t exp = jget_int(yyjson_doc_get_root(doc), "exp", -1);
    yyjson_doc_free(doc);
    return exp;
}

/* ---------- resolution ---------- */

void tny_codex_creds_free(tny_codex_creds *c) {
    if (!c) return;
    if (c->access_token) secure_free(c->access_token);
    if (c->api_key) secure_free(c->api_key);
    free(c->account_id);
    memset(c, 0, sizeof *c);
}

static void set_token(tny_codex_creds *c, const char *token, const char *account,
                      tny_codex_cred_source src) {
    c->access_token = xstrdup(token);
    c->account_id = account && *account ? xstrdup(account) : jwt_account_id(token);
    c->source = src;
}

/* Read one Codex-shaped auth file. 0 when it yields a credential. */
static int read_auth_file(char *path, tny_codex_creds *c, tny_codex_cred_source src) {
    if (!path) return -1;
    yyjson_doc *doc = jparse_file(path);
    free(path);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tokens = jget(root, "tokens");
    const char *access = jget_str(tokens, "access_token");
    if (access && *access) {
        set_token(c, access, jget_str(tokens, "account_id"), src);
        if (!c->account_id) c->account_id = jwt_account_id(jget_str(tokens, "id_token"));
    }
    const char *key = jget_str(root, "OPENAI_API_KEY");
    if (key && *key && !c->access_token) {
        c->api_key = xstrdup(key);
        c->source = src;
    }
    yyjson_doc_free(doc);
    return c->access_token || c->api_key ? 0 : -1;
}

int tny_codex_credentials(const tny_ctx *ctx, tny_codex_creds *c) {
    memset(c, 0, sizeof *c);
    if (ctx && ctx->chatgpt_token && *ctx->chatgpt_token) {
        set_token(c, ctx->chatgpt_token, ctx->chatgpt_account_id, TNY_CODEX_CRED_FLAG);
        return 0;
    }
    const char *env = env_token();
    if (env) {
        const char *acct = getenv("CHATGPT_ACCOUNT_ID");
        set_token(c, env, ctx && ctx->chatgpt_account_id ? ctx->chatgpt_account_id : acct,
                  TNY_CODEX_CRED_ENV);
        return 0;
    }
    if (read_auth_file(tny_codex_store_path(), c, TNY_CODEX_CRED_TNY_STORE) == 0) return 0;
    if (read_auth_file(tny_codex_auth_path(), c, TNY_CODEX_CRED_CODEX_CLI) == 0) return 0;
    return -1;
}

/* ---------- HTTP ---------- */

int tny_codex_http_post(const char *url, const char *content_type, const char *body, buf_t *out,
                        char *err, size_t errlen) {
    http_conn *c = http_open(url, err, errlen);
    if (!c) return -1;
    buf_t ct;
    buf_init(&ct);
    buf_appendf(&ct, "Content-Type: %s", content_type);
    const char *hdrs[] = {ct.data, "Accept: application/json", NULL};
    const char *prefix = http_prefix(c);
    const char *path = prefix && *prefix ? prefix : "/";
    int status = -1;
    if (http_request(c, "POST", path, hdrs, body, strlen(body)) == 0 &&
        (status = http_read_response(c, CODEX_HTTP_TIMEOUT_MS)) > 0) {
        int64_t deadline = now_ms() + CODEX_HTTP_TIMEOUT_MS;
        for (;;) {
            char tmp[8192];
            ssize_t n = http_body_read(c, tmp, sizeof tmp);
            if (n == 0) break;
            if (n == -2) {
                if (now_ms() > deadline) break;
                struct pollfd pf = {http_fd(c), POLLIN, 0};
                tny_poll(&pf, 1, 500);
                continue;
            }
            if (n < 0) break;
            buf_append(out, tmp, (size_t)n);
        }
    }
    buf_free(&ct);
    http_close(c);
    if (status < 0 && err && !*err) snprintf(err, errlen, "no response from %s", url);
    return status;
}

/* ---------- store ---------- */

static void put_str(yyjson_mut_doc *m, yyjson_mut_val *obj, const char *k, const char *v) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(m, k), yyjson_mut_strcpy(m, v));
}

static int write_secret_file(const char *path, yyjson_mut_doc *m) {
    char *json = jwrite_pretty(m);
    if (!json) return -1;
    char *dir = xstrdup(path);
    if (dir) {
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            mkdir_p(dir);
        }
        free(dir);
    }
    int rc = file_write_atomic(path, json, strlen(json));
    if (rc == 0) chmod(path, 0600);
    secure_free(json);
    return rc;
}

/* Apply an OAuth token response onto a Codex-shaped document root: rotate
 * the tokens, keep the account id current, stamp last_refresh/expires_at. */
static bool apply_token_response(yyjson_mut_doc *m, yyjson_mut_val *root, yyjson_val *resp) {
    const char *access = jget_str(resp, "access_token");
    if (!access || !*access) return false;
    yyjson_mut_val *tokens = yyjson_mut_obj_get(root, "tokens");
    if (!yyjson_mut_is_obj(tokens)) {
        tokens = yyjson_mut_obj(m);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(m, "tokens"), tokens);
    }
    put_str(m, tokens, "access_token", access);
    const char *nref = jget_str(resp, "refresh_token");
    if (nref && *nref) put_str(m, tokens, "refresh_token", nref);
    const char *idt = jget_str(resp, "id_token");
    if (idt && *idt) put_str(m, tokens, "id_token", idt);
    char *acct = jwt_account_id(access);
    if (!acct) acct = jwt_account_id(idt);
    if (acct) {
        put_str(m, tokens, "account_id", acct);
        free(acct);
    }
    int64_t now = now_ms() / 1000;
    char ts[32];
    iso8601_from_epoch(now, ts);
    put_str(m, root, "last_refresh", ts);
    int64_t expires_in = jget_int(resp, "expires_in", 0);
    if (expires_in > 0) {
        iso8601_from_epoch(now + expires_in, ts);
        put_str(m, root, "expires_at", ts);
    }
    return true;
}

int tny_codex_store_save(yyjson_val *token_response) {
    char *path = tny_codex_store_path();
    if (!path) return -1;
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    if (!m) {
        free(path);
        return -1;
    }
    yyjson_mut_val *root = yyjson_mut_obj(m);
    yyjson_mut_doc_set_root(m, root);
    put_str(m, root, "auth_mode", "chatgpt");
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(m, "OPENAI_API_KEY"), yyjson_mut_null(m));
    int rc = apply_token_response(m, root, token_response) ? write_secret_file(path, m) : -1;
    yyjson_mut_doc_free(m);
    free(path);
    return rc;
}

/* ---------- refresh ---------- */

static bool file_tokens_stale(yyjson_mut_val *root, yyjson_mut_val *tokens) {
    const char *access = yyjson_mut_get_str(yyjson_mut_obj_get(tokens, "access_token"));
    int64_t now = now_ms() / 1000;
    int64_t exp = jwt_expiry(access);
    if (exp > 0) return now >= exp - CODEX_REFRESH_EARLY_S;
    int64_t at = iso8601_to_epoch(yyjson_mut_get_str(yyjson_mut_obj_get(root, "expires_at")));
    if (at > 0) return now >= at - CODEX_REFRESH_EARLY_S;
    at = iso8601_to_epoch(yyjson_mut_get_str(yyjson_mut_obj_get(root, "last_refresh")));
    return at > 0 && now - at > CODEX_REFRESH_AFTER_S;
}

static const char *refresh_url(void) {
    const char *url = getenv(CODEX_REFRESH_URL_ENV);
    if (url && *url) return url;
    static char built[512];
    const char *iss = getenv("TNY_CODEX_OAUTH_ISSUER");
    snprintf(built, sizeof built, "%s/oauth/token", iss && *iss ? iss : CODEX_OAUTH_ISSUER);
    return built;
}

/* Refresh one Codex-shaped file in place. Returns true when the file held
 * a usable credential at all (stale or not), so the caller stops there. */
static bool refresh_file(char *path) {
    if (!path) return false;
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_doc *old = m ? jparse_file(path) : NULL;
    yyjson_mut_val *root = old ? yyjson_val_mut_copy(m, yyjson_doc_get_root(old)) : NULL;
    yyjson_doc_free(old);
    if (!yyjson_mut_is_obj(root)) {
        yyjson_mut_doc_free(m);
        free(path);
        return false;
    }
    yyjson_mut_val *tokens = yyjson_mut_obj_get(root, "tokens");
    const char *key = yyjson_mut_get_str(yyjson_mut_obj_get(root, "OPENAI_API_KEY"));
    const char *access = NULL, *ref = NULL;
    if (yyjson_mut_is_obj(tokens)) {
        access = yyjson_mut_get_str(yyjson_mut_obj_get(tokens, "access_token"));
        ref = yyjson_mut_get_str(yyjson_mut_obj_get(tokens, "refresh_token"));
    }
    bool usable = (access && *access) || (key && *key);
    if (!usable || !access || !*access || !ref || !*ref || !file_tokens_stale(root, tokens)) {
        yyjson_mut_doc_free(m);
        free(path);
        return usable;
    }
    yyjson_mut_doc_set_root(m, root);

    buf_t body, reply;
    buf_init(&body);
    buf_init(&reply);
    buf_appends(&body, "{\"client_id\":\"" CODEX_OAUTH_CLIENT_ID
                       "\",\"grant_type\":\"refresh_token\",\"refresh_token\":");
    jescape(&body, ref);
    buf_appends(&body, "}");
    char err[256] = "";
    int status =
        tny_codex_http_post(refresh_url(), "application/json", body.data, &reply, err, sizeof err);
    secure_zero(body.data, body.len);
    buf_free(&body);

    if (status >= 200 && status < 300) {
        yyjson_doc *tok = jparse(reply.data, reply.len);
        if (tok && apply_token_response(m, root, yyjson_doc_get_root(tok)))
            write_secret_file(path, m);
        yyjson_doc_free(tok);
    } else if (tny_debug()) {
        fprintf(stderr, "tny: codex token refresh failed (HTTP %d): %s\n", status, err);
    }
    if (reply.data) secure_zero(reply.data, reply.len);
    buf_free(&reply);
    yyjson_mut_doc_free(m);
    free(path);
    return true;
}

/* Best-effort: any failure leaves the file untouched and the stale token
 * flows as before (the provider's 401 then names the fix). Flag/env tokens
 * carry no refresh token and are never refreshed. */
void tny_codex_refresh_if_stale(void) {
    if (env_token()) return;
    if (refresh_file(tny_codex_store_path())) return;
    refresh_file(tny_codex_auth_path());
}

/* ---------- logout ---------- */

int tny_codex_logout(void) {
    char *path = tny_codex_store_path();
    if (!path) return 1;
    int rc = 0;
    if (file_exists(path)) {
        rc = unlink(path) == 0 ? 0 : 1;
        if (rc == 0) printf("codex login removed (%s deleted).\n", path);
        else fprintf(stderr, "tny: cannot remove %s\n", path);
    } else {
        printf("no tny codex login (%s missing).\n", path);
    }
    free(path);
    char *cli = tny_codex_auth_path();
    if (cli && file_exists(cli))
        printf("The Codex CLI's own login (%s) is still present — tny keeps reading it; "
               "run `codex logout` to drop that one.\n",
               cli);
    free(cli);
    if (env_token())
        printf("CHATGPT_ACCESS_TOKEN is still set — unset it to drop the env credential.\n");
    return rc;
}
