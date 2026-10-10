/* openai_auth.c — Sign in with ChatGPT credentials for the builtin openai
 * provider (docs/adr/0186, docs/backends/openai-chatgpt.md).
 *
 * One registration per host, saved in ~/.tny/openai-auth.json (0600):
 *
 *   { "version": 1, "issuer": "https://auth.openai.com",
 *     "client_id": "oaiapp_…", "subject": "<id-token sub>", "email": "…",
 *     "ext_agent_host_id": "urn:uuid:…",
 *     "id_token": "…", "access_token": "…", "refresh_token": "…",
 *     "token_type": "Bearer", "scopes": ["chatgpt.tokens.use.direct", …],
 *     "plan_usage": true, "expires_in": 3600,
 *     "expires_at": "2026-…Z", "saved_at": "2026-…Z" }
 *
 * The shape follows the SIWC credential record. Logout clears the tokens and
 * keeps the issued client id, subject and email, so the next sign-in reuses
 * the registration; `logout --forget` deletes the file. The host id lives in
 * ~/.tny/openai-host-id so it survives both.
 *
 * Refresh tokens rotate and are single-use, so a refresh holds an flock on
 * ~/.tny/openai-auth.lock and re-reads the store under it: a second process
 * that waited sees the first one's fresh token and does not spend the
 * already-rotated refresh token. Never print tokens (CLAUDE.md). */
#include "core/openai_auth.h"
#include "json/json.h"
#include "net/net.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/file.h>
#endif

#define OPENAI_LOCK_WAIT_MS     35000 /* outlasts one 30 s token request */
#define OPENAI_REVOKE_ATTEMPTS  3
#define OPENAI_STORE_VERSION    1
#define OPENAI_ID_TOKEN_SKEW_S  300

/* ---------- paths, host id, issuer ---------- */

static char *tny_file(const char *name) {
    char *dir = path_tny_dir();
    if (!dir) return NULL;
    char *p = path_join(dir, name);
    free(dir);
    return p;
}

char *tny_openai_store_path(void) { return tny_file("openai-auth.json"); }
char *tny_openai_host_id_path(void) { return tny_file("openai-host-id"); }

static bool host_id_valid(const char *s) {
    /* urn:uuid: + 8-4-4-4-12 lowercase hex */
    if (!str_starts(s, "urn:uuid:")) return false;
    s += 9;
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[36] == 0;
}

char *tny_openai_host_id(void) {
    char *path = tny_openai_host_id_path();
    if (!path) return NULL;
    size_t len = 0;
    char *raw = file_slurp(path, &len);
    if (raw) {
        str_trim(raw);
        if (host_id_valid(raw)) {
            free(path);
            return raw;
        }
        free(raw);
        fprintf(stderr, "tny: %s is not a urn:uuid host id; delete it to create a new one\n",
                path);
        free(path);
        return NULL;
    }
    uint8_t b[16];
    if (!random_bytes(b, sizeof b)) {
        fprintf(stderr, "tny: no CSPRNG available (/dev/urandom); cannot create a host id\n");
        free(path);
        return NULL;
    }
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* RFC 9562 version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10 */
    char id[64];
    snprintf(id, sizeof id,
             "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
    char *dir = path_tny_dir();
    if (dir) mkdir_p(dir);
    free(dir);
    char line[72];
    int n = snprintf(line, sizeof line, "%s\n", id);
    if (file_write_atomic(path, line, (size_t)n) != 0) {
        fprintf(stderr, "tny: cannot write %s\n", path);
        free(path);
        return NULL;
    }
    chmod(path, 0600);
    free(path);
    return xstrdup(id);
}

bool tny_loopback_url_valid(const char *url) {
    if (!url || !*url) return true;
    const char *p;
    if (strncmp(url, "http://127.0.0.1", 16) == 0) p = url + 16;
    else if (strncmp(url, "https://127.0.0.1", 17) == 0) p = url + 17;
    else return false;
    if (*p == ':') {
        p++;
        unsigned port = 0;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned)(*p++ - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    if (*p && *p != '/') return false;
    for (; *p; p++)
        if ((unsigned char)*p <= 32 || *p == 127 || *p == '\\' || *p == '#') return false;
    return true;
}

const char *tny_openai_issuer(void) {
    const char *v = getenv("TNY_OPENAI_OAUTH_ISSUER");
    if (!v || !*v) return TNY_OPENAI_SIGNIN_ISSUER;
    return tny_loopback_url_valid(v) ? v : NULL;
}

const char *tny_openai_signin_base_url(void) {
    const char *v = getenv("TNY_OPENAI_SIGNIN_BASE_URL");
    if (!v || !*v) return TNY_OPENAI_API_BASE_URL;
    return tny_loopback_url_valid(v) ? v : NULL;
}

static bool same_issuer(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == '/') la--;
    while (lb && b[lb - 1] == '/') lb--;
    return la == lb && memcmp(a, b, la) == 0;
}

/* ---------- lock ---------- */

#ifndef __EMSCRIPTEN__
/* fd holding the exclusive lock, or -1 when it could not be had in time. */
static int store_lock(void) {
    char *dir = path_tny_dir();
    if (dir) mkdir_p(dir);
    free(dir);
    char *path = tny_file("openai-auth.lock");
    if (!path) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(path);
    if (fd < 0) return -1;
    int64_t deadline = monotonic_ms() + OPENAI_LOCK_WAIT_MS;
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if ((errno != EWOULDBLOCK && errno != EINTR) || monotonic_ms() > deadline) {
            close(fd);
            return -1;
        }
        tny_poll(NULL, 0, 50);
    }
    return fd;
}

static void store_unlock(int fd) {
    if (fd >= 0) close(fd); /* releases the flock */
}
#else
/* One browser tab, one process: nothing to serialize against. */
static int store_lock(void) { return 0; }
static void store_unlock(int fd) { (void)fd; }
#endif

/* ---------- store ---------- */

static yyjson_mut_doc *store_load(const char *path) {
    yyjson_doc *old = jparse_file(path);
    yyjson_val *root = old ? yyjson_doc_get_root(old) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(old);
        return NULL;
    }
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *copy = m ? yyjson_val_mut_copy(m, root) : NULL;
    yyjson_doc_free(old);
    if (!copy) {
        yyjson_mut_doc_free(m);
        return NULL;
    }
    yyjson_mut_doc_set_root(m, copy);
    return m;
}

static void put_str(yyjson_mut_doc *m, yyjson_mut_val *obj, const char *k, const char *v) {
    yyjson_mut_obj_remove_key(obj, k);
    if (v) yyjson_mut_obj_add(obj, yyjson_mut_strcpy(m, k), yyjson_mut_strcpy(m, v));
}

static const char *mut_str(yyjson_mut_val *obj, const char *k) {
    return yyjson_mut_get_str(yyjson_mut_obj_get(obj, k));
}

static int write_store(const char *path, yyjson_mut_doc *m) {
    char *json = jwrite_pretty(m);
    if (!json) return -1;
    char *dir = path_tny_dir();
    if (dir) mkdir_p(dir);
    free(dir);
    int rc = file_write_atomic(path, json, strlen(json)); /* created 0600 */
    if (rc == 0) chmod(path, 0600);
    secure_free(json);
    return rc;
}

static bool scopes_grant_plan(const char *scope) {
    size_t n = strlen(TNY_OPENAI_SIGNIN_SCOPE);
    for (const char *p = scope; p && *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && memcmp(p, TNY_OPENAI_SIGNIN_SCOPE, n) == 0) return true;
        p = e;
    }
    return false;
}

/* Rotate tokens from a token-endpoint response onto the record. A refresh
 * response without `scope` keeps the granted set (refresh omits scope). */
static bool apply_tokens(yyjson_mut_doc *m, yyjson_mut_val *root, yyjson_val *resp) {
    const char *access = jget_str(resp, "access_token");
    if (!access || !*access) return false;
    put_str(m, root, "access_token", access);
    const char *ref = jget_str(resp, "refresh_token");
    if (ref && *ref) put_str(m, root, "refresh_token", ref);
    const char *idt = jget_str(resp, "id_token");
    if (idt && *idt) put_str(m, root, "id_token", idt);
    const char *tt = jget_str(resp, "token_type");
    put_str(m, root, "token_type", tt && *tt ? tt : "Bearer");
    const char *scope = jget_str(resp, "scope");
    if (scope) {
        yyjson_mut_val *arr = yyjson_mut_arr(m);
        for (const char *p = scope; *p;) {
            while (*p == ' ') p++;
            const char *e = p;
            while (*e && *e != ' ') e++;
            if (e > p) yyjson_mut_arr_add_strncpy(m, arr, p, (size_t)(e - p));
            p = e;
        }
        yyjson_mut_obj_remove_key(root, "scopes");
        yyjson_mut_obj_add(root, yyjson_mut_strcpy(m, "scopes"), arr);
        yyjson_mut_obj_remove_key(root, "plan_usage");
        yyjson_mut_obj_add_bool(m, root, "plan_usage", scopes_grant_plan(scope));
    }
    int64_t now = now_ms() / 1000;
    char ts[32];
    iso8601_from_epoch(now, ts);
    put_str(m, root, "saved_at", ts);
    int64_t expires_in = jget_int(resp, "expires_in", 0);
    yyjson_mut_obj_remove_key(root, "expires_in");
    yyjson_mut_obj_remove_key(root, "expires_at");
    if (expires_in > 0) {
        yyjson_mut_obj_add_int(m, root, "expires_in", expires_in);
        iso8601_from_epoch(now + expires_in, ts);
        put_str(m, root, "expires_at", ts);
    }
    return true;
}

static void clear_tokens(yyjson_mut_val *root) {
    static const char *const keys[] = {"access_token", "refresh_token", "id_token",
                                       "token_type",   "expires_in",    "expires_at"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        yyjson_mut_obj_remove_key(root, keys[i]);
}

static int64_t record_expiry(yyjson_mut_val *root) {
    int64_t at = iso8601_to_epoch(mut_str(root, "expires_at"));
    if (at > 0) return at;
    yyjson_doc *claims = jwt_payload_doc(mut_str(root, "access_token"));
    at = claims ? jget_int(yyjson_doc_get_root(claims), "exp", 0) : 0;
    yyjson_doc_free(claims);
    return at;
}

void tny_openai_signin_free(tny_openai_signin *s) {
    if (!s) return;
    free(s->client_id);
    free(s->email);
    free(s->subject);
    if (s->access_token) secure_free(s->access_token);
    memset(s, 0, sizeof *s);
}

static bool dup_into(char **slot, const char *v) {
    if (!v || !*v) return true;
    *slot = xstrdup(v);
    return *slot != NULL;
}

int tny_openai_signin_read(tny_openai_signin *out) {
    memset(out, 0, sizeof *out);
    char *path = tny_openai_store_path();
    if (!path) return -1;
    yyjson_mut_doc *m = store_load(path);
    free(path);
    if (!m) return -1;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    const char *iss = mut_str(root, "issuer");
    const char *want = tny_openai_issuer();
    /* A record from another issuer (a test fixture, say) never authorizes
     * requests or refreshes against this one. */
    if (!want || (iss && !same_issuer(iss, want))) {
        yyjson_mut_doc_free(m);
        return -1;
    }
    int rc = 0;
    if (!dup_into(&out->client_id, mut_str(root, "client_id")) ||
        !dup_into(&out->email, mut_str(root, "email")) ||
        !dup_into(&out->subject, mut_str(root, "subject")) ||
        !dup_into(&out->access_token, mut_str(root, "access_token")))
        rc = -3;
    const char *ref = mut_str(root, "refresh_token");
    out->has_refresh = ref && *ref;
    out->plan_usage = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "plan_usage"));
    out->expires_at = record_expiry(root);
    yyjson_mut_doc_free(m);
    if (rc == 0 && (!out->client_id || !out->access_token)) rc = -1;
    else if (rc == 0 && !out->plan_usage) rc = -2;
    if (rc == -3) tny_openai_signin_free(out);
    return rc;
}

bool tny_openai_signin_present(void) {
    tny_openai_signin s;
    bool ok = tny_openai_signin_read(&s) == 0;
    tny_openai_signin_free(&s);
    return ok;
}

char *tny_openai_signin_email(void) {
    tny_openai_signin s;
    (void)tny_openai_signin_read(&s);
    char *email = s.email;
    s.email = NULL;
    tny_openai_signin_free(&s);
    return email;
}

/* ---------- ID token ---------- */

int tny_openai_id_token_check(const char *id_token, const char *issuer, const char *client_id,
                              const char *nonce, char **sub, char **email, const char **why) {
    *sub = *email = NULL;
    *why = NULL;
    if (!id_token || !*id_token) {
        *why = "the token response has no id_token";
        return -1;
    }
    yyjson_doc *doc = jwt_payload_doc(id_token);
    yyjson_val *c = doc ? yyjson_doc_get_root(doc) : NULL;
    int rc = -1;
    yyjson_val *aud = jget(c, "aud");
    bool aud_ok = false;
    if (yyjson_is_str(aud)) aud_ok = strcmp(yyjson_get_str(aud), client_id) == 0;
    else if (yyjson_is_arr(aud)) {
        size_t i, max;
        yyjson_val *v;
        yyjson_arr_foreach(aud, i, max, v) {
            if (yyjson_is_str(v) && strcmp(yyjson_get_str(v), client_id) == 0) aud_ok = true;
        }
    }
    const char *s = jget_str(c, "sub");
    const char *n = jget_str(c, "nonce");
    int64_t exp = jget_int(c, "exp", 0);
    int64_t now = now_ms() / 1000;
    if (!yyjson_is_obj(c)) *why = "the id_token is not a JWT";
    else if (!same_issuer(jget_str(c, "iss"), issuer)) *why = "the id_token issuer does not match";
    else if (!aud_ok) *why = "the id_token audience is not this client";
    else if (exp <= 0 || exp + OPENAI_ID_TOKEN_SKEW_S < now) *why = "the id_token has expired";
    else if (!n || strcmp(n, nonce) != 0) *why = "the id_token nonce does not match this sign-in";
    else if (!s || !*s) *why = "the id_token has no subject";
    else {
        *sub = xstrdup(s);
        const char *e = jget_str(c, "email");
        if (e && *e) *email = xstrdup(e);
        rc = *sub ? 0 : -1;
        if (rc) *why = "out of memory";
    }
    yyjson_doc_free(doc);
    return rc;
}

int tny_openai_store_save_login(yyjson_val *token_response, const char *client_id,
                                const char *subject, const char *email) {
    char *path = tny_openai_store_path();
    char *host = tny_openai_host_id();
    const char *iss = tny_openai_issuer();
    if (!path || !host || !iss) {
        free(path);
        free(host);
        return -1;
    }
    int lock = store_lock();
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = m ? yyjson_mut_obj(m) : NULL;
    int rc = -1;
    if (root) {
        yyjson_mut_doc_set_root(m, root);
        yyjson_mut_obj_add_int(m, root, "version", OPENAI_STORE_VERSION);
        put_str(m, root, "issuer", iss);
        put_str(m, root, "client_id", client_id);
        put_str(m, root, "subject", subject);
        put_str(m, root, "email", email);
        put_str(m, root, "ext_agent_host_id", host);
        if (apply_tokens(m, root, token_response)) rc = write_store(path, m);
    }
    yyjson_mut_doc_free(m);
    store_unlock(lock);
    free(host);
    free(path);
    return rc;
}

/* ---------- HTTP (form posts to the issuer) ---------- */

static int form_post(const char *what, const buf_t *form, buf_t *reply) {
    const char *iss = tny_openai_issuer();
    if (!iss) return -1;
    buf_t url;
    buf_init(&url);
    buf_appendf(&url, "%s/api/accounts/oauth/%s", iss, what);
    char err[256] = "";
    int status = tny_codex_http_post(url.data, "application/x-www-form-urlencoded", form->data,
                                     reply, err, sizeof err);
    if (status < 0 && tny_debug()) fprintf(stderr, "tny: %s: %s\n", url.data, err);
    buf_free(&url);
    return status;
}

static const char *oauth_error_code(const buf_t *body, yyjson_doc **doc) {
    *doc = jparse(body->data, body->len);
    yyjson_val *root = *doc ? yyjson_doc_get_root(*doc) : NULL;
    yyjson_val *e = jget(root, "error");
    if (yyjson_is_str(e)) return yyjson_get_str(e);
    return jget_str(e, "code");
}

static bool refresh_error_terminal(const char *code) {
    static const char *const terminal[] = {
        "invalid_grant",         "invalid_refresh_token",     "token_expired",
        "refresh_token_expired", "refresh_token_invalidated", "refresh_token_reused"};
    for (size_t i = 0; code && i < sizeof terminal / sizeof terminal[0]; i++)
        if (strcmp(code, terminal[i]) == 0) return true;
    return false;
}

/* ---------- refresh ---------- */

void tny_openai_refresh_if_stale(void) {
    char *path = tny_openai_store_path();
    if (!path || !file_exists(path)) {
        free(path);
        return;
    }
    int lock = store_lock();
    if (lock < 0) { /* someone else is mid-refresh for too long: use what is there */
        free(path);
        return;
    }
    yyjson_mut_doc *m = store_load(path); /* re-read under the lock */
    yyjson_mut_val *root = m ? yyjson_mut_doc_get_root(m) : NULL;
    const char *iss = mut_str(root, "issuer");
    const char *client = mut_str(root, "client_id");
    const char *ref = mut_str(root, "refresh_token");
    const char *access = mut_str(root, "access_token");
    int64_t exp = root ? record_expiry(root) : 0;
    bool stale = access && *access && exp > 0 && now_ms() / 1000 >= exp - TNY_OPENAI_REFRESH_EARLY_S;
    bool ours = tny_openai_issuer() && (!iss || same_issuer(iss, tny_openai_issuer()));
    if (!root || !ours || !stale || !client || !*client || !ref || !*ref) {
        yyjson_mut_doc_free(m);
        store_unlock(lock);
        free(path);
        return;
    }
    buf_t form, reply;
    buf_init(&form);
    buf_init(&reply);
    url_form_append(&form, "grant_type", "refresh_token");
    url_form_append(&form, "client_id", client);
    url_form_append(&form, "refresh_token", ref);
    url_form_append(&form, "resource", TNY_OPENAI_API_BASE_URL);
    int status = form_post("token", &form, &reply);
    secure_zero(form.data, form.len);
    buf_free(&form);
    if (status >= 200 && status < 300) {
        yyjson_doc *tok = jparse(reply.data, reply.len);
        if (tok && apply_tokens(m, root, yyjson_doc_get_root(tok))) write_store(path, m);
        else fprintf(stderr, "tny: ChatGPT token refresh returned no access_token\n");
        yyjson_doc_free(tok);
    } else if (status >= 400 && status < 500) {
        yyjson_doc *edoc = NULL;
        const char *code = oauth_error_code(&reply, &edoc);
        if (refresh_error_terminal(code)) {
            /* The renewable session is gone. Keep the registration for the
             * next sign-in; drop the tokens nobody can use. */
            clear_tokens(root);
            write_store(path, m);
            fprintf(stderr,
                    "tny: the ChatGPT sign-in expired (%s); run `tny --provider openai login`\n",
                    code);
        } else {
            fprintf(stderr, "tny: ChatGPT token refresh failed (HTTP %d%s%s)\n", status,
                    code ? ", " : "", code ? code : "");
        }
        yyjson_doc_free(edoc);
    } else if (tny_debug()) {
        fprintf(stderr, "tny: ChatGPT token refresh failed (HTTP %d); keeping the session\n",
                status);
    }
    if (reply.data) secure_zero(reply.data, reply.len);
    buf_free(&reply);
    yyjson_mut_doc_free(m);
    store_unlock(lock);
    free(path);
}

static void signin_drop(tny_ctx *ctx, bool ours) {
    if (ours) { /* the plan token must not ride anywhere else */
        secure_free(ctx->api_key);
        ctx->api_key = NULL;
    }
    ctx->openai_signin = false;
    ctx->openai_signin_expires_at = 0;
    ctx->openai_signin_key = 0;
}

void tny_openai_signin_sync(tny_ctx *ctx) {
    if (!ctx || !ctx->openai_signin) return;
    /* Flags applied after resolution (--api-key-env, --base-url, --wire-api)
     * or an embedder may have replaced the key or moved the endpoint. */
    bool ours = ctx->api_key && fnv1a(ctx->api_key, strlen(ctx->api_key)) == ctx->openai_signin_key;
    const char *base = tny_openai_signin_base_url();
    bool endpoint = base && ctx->base_url && strcmp(ctx->base_url, base) == 0 &&
                    !tny_wire_is_chat(ctx->wire_api) && ctx->auth_header_name &&
                    strcasecmp(ctx->auth_header_name, "Authorization") == 0;
    if (!ours || !endpoint) {
        signin_drop(ctx, ours);
        return;
    }
    int64_t now = now_ms() / 1000;
    if (ctx->openai_signin_expires_at > 0 &&
        now < ctx->openai_signin_expires_at - TNY_OPENAI_REFRESH_EARLY_S)
        return;
    tny_openai_refresh_if_stale();
    tny_openai_signin s;
    int rc = tny_openai_signin_read(&s);
    if (rc == 0) {
        if (strcmp(ctx->api_key, s.access_token) != 0) {
            secure_free(ctx->api_key);
            ctx->api_key = s.access_token;
            s.access_token = NULL;
            ctx->openai_signin_key = fnv1a(ctx->api_key, strlen(ctx->api_key));
        }
        ctx->openai_signin_expires_at = s.expires_at;
    } else if (rc != -3) {
        /* signed out or session cleared: the request must not carry it */
        signin_drop(ctx, true);
    }
    tny_openai_signin_free(&s);
}

/* ---------- logout ---------- */

/* RFC 7009 revocation of the renewable session. An empty 200 is success,
 * also for an already-invalid token; network failures and 5xx retry. */
static bool revoke_session(const char *client_id, const char *refresh_token) {
    for (int attempt = 0; attempt < OPENAI_REVOKE_ATTEMPTS; attempt++) {
        if (attempt) tny_poll(NULL, 0, 500 << attempt);
        buf_t form, reply;
        buf_init(&form);
        buf_init(&reply);
        url_form_append(&form, "token", refresh_token);
        url_form_append(&form, "token_type_hint", "refresh_token");
        url_form_append(&form, "client_id", client_id);
        int status = form_post("revoke", &form, &reply);
        secure_zero(form.data, form.len);
        buf_free(&form);
        buf_free(&reply);
        if (status >= 200 && status < 300) return true;
        if (status >= 400 && status < 500) {
            fprintf(stderr, "tny: token revocation refused (HTTP %d)\n", status);
            return false;
        }
    }
    return false;
}

int tny_openai_logout(bool forget) {
    char *path = tny_openai_store_path();
    if (!path) return 1;
    if (!file_exists(path)) {
        printf("no ChatGPT sign-in for openai (%s missing).\n", path);
        free(path);
        return 0;
    }
    int lock = store_lock();
    yyjson_mut_doc *m = store_load(path);
    yyjson_mut_val *root = m ? yyjson_mut_doc_get_root(m) : NULL;
    const char *client = mut_str(root, "client_id");
    const char *ref = mut_str(root, "refresh_token");
    const char *iss = mut_str(root, "issuer");
    bool revoked = true;
    if (client && *client && ref && *ref && tny_openai_issuer() &&
        (!iss || same_issuer(iss, tny_openai_issuer())))
        revoked = revoke_session(client, ref);
    int rc = 0;
    if (forget || !root) {
        if (unlink(path) != 0) {
            fprintf(stderr, "tny: cannot remove %s\n", path);
            rc = 1;
        } else {
            printf("Signed out of ChatGPT; registration forgotten (%s deleted).\n", path);
        }
    } else {
        clear_tokens(root);
        if (write_store(path, m) != 0) {
            fprintf(stderr, "tny: cannot rewrite %s\n", path);
            rc = 1;
        } else {
            printf("Signed out of ChatGPT. The app registration stays in %s for the next "
                   "`tny --provider openai login` (`logout --forget` deletes it).\n",
                   path);
        }
    }
    if (!revoked)
        printf("Remote revocation was not confirmed. To cut off this session now, disconnect "
               "tny under ChatGPT Settings → Apps (%s).\n",
               TNY_CHATGPT_USAGE_URL);
    yyjson_mut_doc_free(m);
    store_unlock(lock);
    free(path);
    const char *key = getenv("OPENAI_API_KEY");
    if (key && *key) printf("OPENAI_API_KEY is still set; the openai provider keeps using it.\n");
    return rc;
}

/* ---------- request shaping ---------- */

bool tny_openai_signin_mode(const tny_ctx *ctx) {
    if (!ctx) return false;
    if (ctx->openai_signin) return true;
    /* Pi's rule: OpenAI API keys start with `sk-`; any other bearer sent
     * straight to api.openai.com is a ChatGPT-plan access token (a
     * subagent or job child, or a library caller that holds one). */
    if (!ctx->api_key || !*ctx->api_key || str_starts(ctx->api_key, "sk-")) return false;
    if (ctx->auth_header_name && strcasecmp(ctx->auth_header_name, "Authorization") != 0)
        return false;
    url_parts u;
    if (!ctx->base_url || url_parse(ctx->base_url, &u) != 0) return false;
    return strcmp(u.scheme, "https") == 0 && strcasecmp(u.host, "api.openai.com") == 0;
}
