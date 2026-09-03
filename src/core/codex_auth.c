/* codex_auth.c — ChatGPT subscription credentials for the builtin codex
 * profile (docs/adr/0065, docs/backends/codex.md).
 *
 * `codex login` (the Codex CLI) drops `$CODEX_HOME/auth.json` (default
 * `~/.codex/auth.json`):
 *
 *   { "auth_mode": "chatgpt",
 *     "OPENAI_API_KEY": null,
 *     "tokens": { "id_token": "<jwt>", "access_token": "<jwt>",
 *                 "refresh_token": "…", "account_id": "…" },
 *     "last_refresh": "2026-01-01T00:00:00Z" }
 *
 * The access token is the bearer for chatgpt.com/backend-api/codex; the
 * ChatGPT account id (tokens.account_id, else the
 * `https://api.openai.com/auth`.chatgpt_account_id JWT claim) rides the
 * `chatgpt-account-id` header. An `OPENAI_API_KEY` entry (auth_mode
 * "apikey") means the plain OpenAI API instead.
 *
 * Refresh mirrors the Codex CLI: POST JSON to auth.openai.com/oauth/token
 * with the shared public client id, grant_type refresh_token, when the
 * access token's `exp` is at/near now or last_refresh is older than eight
 * days; the rotated tokens are written back into the same file so both
 * tools keep sharing one session. Never log tokens (CLAUDE.md). */
#include "core/config.h"
#include "json/json.h"
#include "net/net.h"
#include "util/util.h"
#include "util/tny_poll.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_REFRESH_URL     "https://auth.openai.com/oauth/token"
/* Same override the Codex CLI honors (its own test suite uses it). */
#define CODEX_REFRESH_URL_ENV "CODEX_REFRESH_TOKEN_URL_OVERRIDE"
#define CODEX_AUTH_CLAIM      "https://api.openai.com/auth"
#define CODEX_REFRESH_AFTER_S (8 * 24 * 3600) /* last_refresh age, like the CLI */
#define CODEX_REFRESH_EARLY_S 60              /* before `exp`, never mid-turn */
#define CODEX_HTTP_TIMEOUT_MS 30000

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

/* A `codex login` drops auth.json under $CODEX_HOME. Its presence means the
 * user's ChatGPT subscription can drive the codex profile with no API key. */
bool tny_codex_auth_present(void) {
    char *p = tny_codex_auth_path();
    if (!p) return false;
    bool ok = file_exists(p);
    free(p);
    return ok;
}

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

void tny_codex_creds_free(tny_codex_creds *c) {
    if (!c) return;
    if (c->access_token) secure_free(c->access_token);
    if (c->api_key) secure_free(c->api_key);
    free(c->account_id);
    memset(c, 0, sizeof *c);
}

int tny_codex_credentials(tny_codex_creds *c) {
    memset(c, 0, sizeof *c);
    char *path = tny_codex_auth_path();
    if (!path) return -1;
    yyjson_doc *doc = jparse_file(path);
    free(path);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tokens = jget(root, "tokens");
    const char *access = jget_str(tokens, "access_token");
    if (access && *access) {
        c->access_token = xstrdup(access);
        const char *acct = jget_str(tokens, "account_id");
        if (acct && *acct) c->account_id = xstrdup(acct);
        if (!c->account_id) c->account_id = jwt_account_id(access);
        if (!c->account_id) c->account_id = jwt_account_id(jget_str(tokens, "id_token"));
    }
    const char *key = jget_str(root, "OPENAI_API_KEY");
    if (key && *key) c->api_key = xstrdup(key);
    yyjson_doc_free(doc);
    return c->access_token || c->api_key ? 0 : -1;
}

/* ---------- refresh ---------- */

static void put_str(yyjson_mut_doc *m, yyjson_mut_val *obj, const char *k, const char *v) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(m, k), yyjson_mut_strcpy(m, v));
}

/* POST a JSON body, slurp the JSON reply. HTTP status, or -1 with err. */
static int post_json(const char *url, const char *body, buf_t *out, char *err, size_t errlen) {
    http_conn *c = http_open(url, err, errlen);
    if (!c) return -1;
    const char *hdrs[] = {"Content-Type: application/json", "Accept: application/json", NULL};
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
    http_close(c);
    if (status < 0 && err && !*err) snprintf(err, errlen, "no response from %s", url);
    return status;
}

static bool codex_tokens_stale(yyjson_mut_val *tokens, yyjson_mut_val *root) {
    const char *access = yyjson_mut_get_str(yyjson_mut_obj_get(tokens, "access_token"));
    int64_t now = now_ms() / 1000;
    int64_t exp = jwt_expiry(access);
    if (exp > 0) return now >= exp - CODEX_REFRESH_EARLY_S;
    const char *last = yyjson_mut_get_str(yyjson_mut_obj_get(root, "last_refresh"));
    int64_t at = iso8601_to_epoch(last);
    return at > 0 && now - at > CODEX_REFRESH_AFTER_S;
}

/* Best-effort: any failure leaves auth.json untouched and the stale token
 * flows as before (the provider's 401 then names the fix). */
void tny_codex_refresh_if_stale(void) {
    char *path = tny_codex_auth_path();
    if (!path) return;
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_doc *old = m ? jparse_file(path) : NULL;
    yyjson_mut_val *root = old ? yyjson_val_mut_copy(m, yyjson_doc_get_root(old)) : NULL;
    yyjson_doc_free(old);
    yyjson_mut_val *tokens = root ? yyjson_mut_obj_get(root, "tokens") : NULL;
    const char *ref = yyjson_mut_get_str(yyjson_mut_obj_get(tokens, "refresh_token"));
    if (!yyjson_mut_is_obj(root) || !yyjson_mut_is_obj(tokens) || !ref || !*ref ||
        !codex_tokens_stale(tokens, root)) {
        yyjson_mut_doc_free(m);
        free(path);
        return;
    }
    yyjson_mut_doc_set_root(m, root);

    buf_t body, reply;
    buf_init(&body);
    buf_init(&reply);
    buf_appends(&body, "{\"client_id\":\"" CODEX_OAUTH_CLIENT_ID
                       "\",\"grant_type\":\"refresh_token\",\"refresh_token\":");
    jescape(&body, ref);
    buf_appends(&body, "}");
    const char *url = getenv(CODEX_REFRESH_URL_ENV);
    if (!url || !*url) url = CODEX_REFRESH_URL;
    char err[256] = "";
    int status = post_json(url, body.data, &reply, err, sizeof err);
    secure_zero(body.data, body.len);
    buf_free(&body);

    if (status >= 200 && status < 300) {
        yyjson_doc *tok = jparse(reply.data, reply.len);
        yyjson_val *troot = tok ? yyjson_doc_get_root(tok) : NULL;
        const char *access = jget_str(troot, "access_token");
        if (access && *access) {
            put_str(m, tokens, "access_token", access);
            const char *nref = jget_str(troot, "refresh_token");
            if (nref && *nref) put_str(m, tokens, "refresh_token", nref);
            const char *idt = jget_str(troot, "id_token");
            if (idt && *idt) put_str(m, tokens, "id_token", idt);
            char ts[32];
            iso8601_from_epoch(now_ms() / 1000, ts);
            put_str(m, root, "last_refresh", ts);
            char *json = jwrite_pretty(m);
            if (json) {
                if (file_write_atomic(path, json, strlen(json)) == 0) chmod(path, 0600);
                secure_free(json);
            }
        }
        yyjson_doc_free(tok);
    } else if (tny_debug()) {
        fprintf(stderr, "tny: codex token refresh failed (HTTP %d): %s\n", status, err);
    }
    if (reply.data) secure_zero(reply.data, reply.len);
    buf_free(&reply);
    yyjson_mut_doc_free(m);
    free(path);
}

/* ---------- logout ---------- */

/* What `codex logout` does: drop the credential file. */
int tny_codex_logout(void) {
    char *path = tny_codex_auth_path();
    if (!path) return 1;
    if (!file_exists(path)) {
        printf("no codex login (%s missing).\n", path);
        free(path);
        return 0;
    }
    int rc = unlink(path) == 0 ? 0 : 1;
    if (rc == 0) printf("codex login removed (%s deleted).\n", path);
    else fprintf(stderr, "tny: cannot remove %s\n", path);
    free(path);
    return rc;
}
