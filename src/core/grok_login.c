/* grok_login.c — native xAI sign-in for the builtin grok profile
 * (docs/adr/0021). No grok CLI required.
 *
 * Implements the RFC 8628 Device Authorization Grant against the xAI
 * OAuth2 provider (grok-build's flow, pinned to its release schema):
 *   POST {issuer}/oauth2/device/code   client_id, scope, referrer form
 *     -> device_code, user_code, verification_uri[_complete],
 *        expires_in, interval
 *   POST {issuer}/oauth2/token         grant_type=…:device_code poll
 *     -> access_token, refresh_token?, expires_in?, id_token?
 *        or {error: authorization_pending | slow_down | access_denied |
 *            expired_token}
 * plus the grant_type=refresh_token exchange the grok CLI would normally
 * run in the background — without it a device-code login dies with the
 * first access token.
 *
 * Credentials land in ~/.grok/auth.json in the grok CLI's own store
 * format (scope key "{issuer}::{client_id}" -> {key, auth_mode:"oidc",
 * create_time, user_id, refresh_token, expires_at, oidc_issuer,
 * oidc_client_id}), so both tools read and refresh the same entry.
 * Never log tokens (CLAUDE.md). */
#include "core/config.h"
#include "json/json.h"
#include "net/net.h"
#include "util/util.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GROK_OAUTH_ISSUER    "https://auth.x.ai"
#define GROK_OAUTH_CLIENT_ID "b1a00492-073a-47ea-816f-4c329264a828"
/* grok-build's frozen default client scope set. */
#define GROK_OAUTH_SCOPES                                                     \
    "openid profile email offline_access grok-cli:access api:access "        \
    "conversations:read conversations:write workspaces:read workspaces:write"
#define GROK_DEVICE_GRANT "urn:ietf:params:oauth:grant-type:device_code"
#define GROK_LEGACY_SCOPE "https://accounts.x.ai/sign-in"

#define OAUTH_HTTP_TIMEOUT_MS   30000
#define DEFAULT_POLL_INTERVAL_S 5
#define SLOW_DOWN_INCREMENT_S   5
#define MIN_CODE_EXPIRY_S       (10 * 60)
/* Refresh this long before expires_at so a token never dies mid-turn. */
#define REFRESH_EARLY_S         60

static const char *grok_issuer(void) {
    const char *v = getenv("GROK_OAUTH2_ISSUER");
    return v && *v ? v : GROK_OAUTH_ISSUER;
}

static const char *grok_client_id(void) {
    const char *v = getenv("GROK_OAUTH2_CLIENT_ID");
    return v && *v ? v : GROK_OAUTH_CLIENT_ID;
}

static char *grok_auth_json_path(void) {
    char *home = path_home();
    if (!home) return NULL;
    char *p = path_join(home, ".grok/auth.json");
    free(home);
    return p;
}

/* ---------- x-www-form-urlencoded ---------- */

static void form_append(buf_t *b, const char *key, const char *val) {
    if (b->len) buf_appends(b, "&");
    buf_appends(b, key);
    buf_appends(b, "=");
    for (const unsigned char *p = (const unsigned char *)val; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~')
            buf_appendf(b, "%c", c);
        else
            buf_appendf(b, "%%%02X", c);
    }
}

/* ---------- ISO-8601 <-> epoch (UTC, whole seconds) ---------- */

static void iso8601_from_epoch(int64_t t, char out[32]) {
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Fractional seconds and anything after them are ignored; auth.x.ai stamps
 * UTC ("…Z"), so no offset handling. -1 on parse failure. */
static int64_t iso8601_to_epoch(const char *s) {
    int y, mo, d, h, mi, sec;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return -1;
    /* days-from-civil (public-domain calendar algorithm) */
    int64_t yy = y - (mo < 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + sec;
}

/* ---------- JWT payload peek (display info only, no verification) ---------- */

/* The id_token arrives over the direct HTTPS channel and only feeds the
 * store's user_id/email display fields, so its signature is not checked
 * (same stance as grok-build). */
static yyjson_doc *jwt_payload(const char *jwt) {
    if (!jwt) return NULL;
    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return NULL;
    const char *start = dot1 + 1;
    const char *dot2 = strchr(start, '.');
    size_t n = dot2 ? (size_t)(dot2 - start) : strlen(start);
    /* base64url -> base64: b64_decode knows only the standard alphabet */
    char *std = xstrndup(start, n);
    if (!std) return NULL;
    for (char *p = std; *p; p++) {
        if (*p == '-') *p = '+';
        else if (*p == '_') *p = '/';
    }
    uint8_t *raw = malloc(n + 4);
    if (!raw) { free(std); return NULL; }
    size_t rn = b64_decode(std, raw, n + 4);
    free(std);
    yyjson_doc *doc = rn ? jparse((const char *)raw, rn) : NULL;
    free(raw);
    return doc;
}

/* ---------- HTTP: POST a form, slurp the JSON body ---------- */

/* Returns the HTTP status, or -1 with err filled. Body bytes land in out. */
static int oauth_post_form(const char *issuer, const char *path,
                           const char *form, buf_t *out,
                           char *err, size_t errlen) {
    http_conn *c = http_open(issuer, err, errlen);
    if (!c) return -1;
    const char *hdrs[] = {"Content-Type: application/x-www-form-urlencoded",
                          "Accept: application/json", NULL};
    buf_t p;
    buf_init(&p);
    buf_appendf(&p, "%s%s", http_prefix(c), path);
    int status = -1;
    if (http_request(c, "POST", p.data, hdrs, form, strlen(form)) == 0 &&
        (status = http_read_response(c, OAUTH_HTTP_TIMEOUT_MS)) > 0) {
        int64_t deadline = now_ms() + OAUTH_HTTP_TIMEOUT_MS;
        for (;;) {
            char tmp[8192];
            ssize_t n = http_body_read(c, tmp, sizeof tmp);
            if (n == 0) break;
            if (n == -2) {
                if (now_ms() > deadline) break;
                struct pollfd pf = {http_fd(c), POLLIN, 0};
                poll(&pf, 1, 500);
                continue;
            }
            if (n < 0) break;
            buf_append(out, tmp, (size_t)n);
        }
    }
    buf_free(&p);
    http_close(c);
    if (status < 0 && err && !*err)
        snprintf(err, errlen, "no response from %s", issuer);
    return status;
}

/* ---------- auth.json store ---------- */

static void put_str(yyjson_mut_doc *m, yyjson_mut_val *obj, const char *k,
                    const char *v) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(m, k), yyjson_mut_strcpy(m, v));
}

static int store_save(yyjson_mut_doc *m, const char *path) {
    char *json = jwrite_pretty(m);
    if (!json) return -1;
    char *dir = xstrdup(path);
    if (dir) {
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir_p(dir); }
        free(dir);
    }
    int rc = file_write_atomic(path, json, strlen(json));
    free(json);
    return rc;
}

/* Load ~/.grok/auth.json as a mutable object root (empty when absent). */
static yyjson_mut_doc *store_load(const char *path) {
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    if (!m) return NULL;
    yyjson_mut_val *root = NULL;
    yyjson_doc *old = jparse_file(path);
    if (old) {
        root = yyjson_val_mut_copy(m, yyjson_doc_get_root(old));
        yyjson_doc_free(old);
    }
    if (!root || !yyjson_mut_is_obj(root)) root = yyjson_mut_obj(m);
    if (!root) { yyjson_mut_doc_free(m); return NULL; }
    yyjson_mut_doc_set_root(m, root);
    return m;
}

/* Write/replace the "{issuer}::{client_id}" entry after a fresh login. */
static int store_put_login(const char *issuer, const char *client_id,
                           yyjson_val *tokens) {
    const char *access = jget_str(tokens, "access_token");
    if (!access || !*access) return -1;
    char *path = grok_auth_json_path();
    if (!path) return -1;
    yyjson_mut_doc *m = store_load(path);
    if (!m) { free(path); return -1; }

    yyjson_mut_val *e = yyjson_mut_obj(m);
    put_str(m, e, "key", access);
    put_str(m, e, "auth_mode", "oidc");
    int64_t now = now_ms() / 1000;
    char ts[32];
    iso8601_from_epoch(now, ts);
    put_str(m, e, "create_time", ts);
    yyjson_doc *claims = jwt_payload(jget_str(tokens, "id_token"));
    yyjson_val *croot = claims ? yyjson_doc_get_root(claims) : NULL;
    const char *sub = jget_str(croot, "sub");
    const char *email = jget_str(croot, "email");
    put_str(m, e, "user_id", sub ? sub : "");
    if (email) put_str(m, e, "email", email);
    yyjson_doc_free(claims);
    const char *refresh = jget_str(tokens, "refresh_token");
    if (refresh) put_str(m, e, "refresh_token", refresh);
    int64_t expires_in = jget_int(tokens, "expires_in", 0);
    if (expires_in > 0) {
        iso8601_from_epoch(now + expires_in, ts);
        put_str(m, e, "expires_at", ts);
    }
    put_str(m, e, "oidc_issuer", issuer);
    put_str(m, e, "oidc_client_id", client_id);

    buf_t scope;
    buf_init(&scope);
    size_t ilen = strlen(issuer);
    while (ilen && issuer[ilen - 1] == '/') ilen--;
    buf_append(&scope, issuer, ilen);
    buf_appendf(&scope, "::%s", client_id);
    yyjson_mut_obj_put(yyjson_mut_doc_get_root(m),
                       yyjson_mut_strcpy(m, scope.data), e);
    buf_free(&scope);

    int rc = store_save(m, path);
    yyjson_mut_doc_free(m);
    free(path);
    return rc;
}

/* ---------- device-code login ---------- */

static void print_oauth_error(const char *what, int status, const buf_t *body) {
    yyjson_doc *doc = jparse(body->data, body->len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *desc = jget_str(root, "error_description");
    if (!desc) desc = jget_str(root, "error");
    fprintf(stderr, "tny: %s failed (HTTP %d)%s%s\n", what, status,
            desc ? ": " : "", desc ? desc : "");
    yyjson_doc_free(doc);
}

int tny_grok_login(void) {
    const char *issuer = grok_issuer();
    const char *client_id = grok_client_id();
    char err[256] = "";

    buf_t form, body;
    buf_init(&form);
    buf_init(&body);
    form_append(&form, "client_id", client_id);
    form_append(&form, "scope", GROK_OAUTH_SCOPES);
    form_append(&form, "referrer", "grok-build");
    int status = oauth_post_form(issuer, "/oauth2/device/code", form.data,
                                 &body, err, sizeof err);
    buf_free(&form);
    if (status < 0) {
        fprintf(stderr, "tny: cannot reach %s: %s\n", issuer, err);
        buf_free(&body);
        return 1;
    }
    if (status == 404) {
        fprintf(stderr, "tny: %s has no device-code endpoint; set XAI_API_KEY "
                        "to use api.x.ai directly.\n", issuer);
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
    char *device_code = NULL, *user_code = NULL, *display_uri = NULL;
    const char *dc = jget_str(root, "device_code");
    const char *uc = jget_str(root, "user_code");
    const char *uri = jget_str(root, "verification_uri_complete");
    if (!uri) uri = jget_str(root, "verification_uri");
    int64_t interval = jget_int(root, "interval", DEFAULT_POLL_INTERVAL_S);
    int64_t code_ttl = jget_int(root, "expires_in", MIN_CODE_EXPIRY_S);
    if (code_ttl < MIN_CODE_EXPIRY_S) code_ttl = MIN_CODE_EXPIRY_S;
    if (dc && uc && uri) {
        device_code = xstrdup(dc);
        user_code = xstrdup(uc);
        display_uri = xstrdup(uri);
    }
    yyjson_doc_free(doc);
    buf_free(&body);
    if (!device_code || !user_code || !display_uri) {
        free(device_code); free(user_code); free(display_uri);
        fprintf(stderr, "tny: malformed device-code response from %s\n", issuer);
        return 1;
    }

    printf("To sign in, open this URL in a browser on any device:\n\n"
           "  %s\n\n"
           "and confirm this code (only continue with a code you requested):\n\n"
           "  %s\n\n"
           "Waiting for authorization…\n", display_uri, user_code);
    fflush(stdout);
    free(display_uri);

    int rc = 1;
    int64_t deadline = now_ms() + code_ttl * 1000;
    if (interval < 0) interval = DEFAULT_POLL_INTERVAL_S;
    for (;;) {
        /* Sleep first: an instant poll on a fresh code only earns an
         * authorization_pending (and risks a slow_down). */
        sleep((unsigned)interval);
        if (now_ms() > deadline) {
            fprintf(stderr, "tny: device code expired; run `tny --provider "
                            "grok login` again.\n");
            break;
        }
        buf_t poll_form, poll_body;
        buf_init(&poll_form);
        buf_init(&poll_body);
        form_append(&poll_form, "grant_type", GROK_DEVICE_GRANT);
        form_append(&poll_form, "device_code", device_code);
        form_append(&poll_form, "client_id", client_id);
        err[0] = 0;
        status = oauth_post_form(issuer, "/oauth2/token", poll_form.data,
                                 &poll_body, err, sizeof err);
        buf_free(&poll_form);
        if (status < 0) {
            fprintf(stderr, "tny: cannot reach %s: %s\n", issuer, err);
            buf_free(&poll_body);
            break;
        }
        if (status >= 200 && status < 300) {
            yyjson_doc *tok = jparse(poll_body.data, poll_body.len);
            buf_free(&poll_body);
            int saved = tok ? store_put_login(issuer, client_id,
                                              yyjson_doc_get_root(tok)) : -1;
            yyjson_doc_free(tok);
            if (saved == 0) {
                printf("grok session saved (~/.grok/auth.json) — "
                       "`tny --provider grok` uses it.\n");
                rc = 0;
            } else {
                fprintf(stderr, "tny: sign-in succeeded but writing "
                                "~/.grok/auth.json failed.\n");
            }
            break;
        }
        yyjson_doc *edoc = jparse(poll_body.data, poll_body.len);
        buf_free(&poll_body);
        const char *e = jget_str(yyjson_doc_get_root(edoc), "error");
        const char *desc =
            jget_str(yyjson_doc_get_root(edoc), "error_description");
        if (e && strcmp(e, "authorization_pending") == 0) {
            yyjson_doc_free(edoc);
            continue;
        }
        if (e && strcmp(e, "slow_down") == 0) {
            interval += SLOW_DOWN_INCREMENT_S;
            yyjson_doc_free(edoc);
            continue;
        }
        if (e && strcmp(e, "access_denied") == 0)
            fprintf(stderr, "tny: authorization denied in the browser.\n");
        else if (e && strcmp(e, "expired_token") == 0)
            fprintf(stderr, "tny: device code expired; run `tny --provider "
                            "grok login` again.\n");
        else
            fprintf(stderr, "tny: token exchange failed (HTTP %d)%s%s\n",
                    status, desc || e ? ": " : "",
                    desc ? desc : (e ? e : ""));
        yyjson_doc_free(edoc);
        break;
    }
    free(device_code);
    free(user_code);
    return rc;
}

/* ---------- refresh ---------- */

/* Refresh the first refreshable expired entry in ~/.grok/auth.json (the one
 * a login above wrote, or the grok CLI's own OIDC entry — the issuer and
 * client_id ride inside the entry). Best-effort: any failure leaves the
 * store untouched and the stale token flows as before. */
void tny_grok_refresh_if_stale(void) {
    char *path = grok_auth_json_path();
    if (!path) return;
    yyjson_mut_doc *m = store_load(path);
    if (!m) { free(path); return; }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);

    yyjson_mut_val *entry = NULL;
    yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(root);
    yyjson_mut_val *k;
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
        if (!yyjson_mut_is_obj(v)) continue;
        const char *exp = yyjson_mut_get_str(yyjson_mut_obj_get(v, "expires_at"));
        const char *ref = yyjson_mut_get_str(yyjson_mut_obj_get(v, "refresh_token"));
        const char *iss = yyjson_mut_get_str(yyjson_mut_obj_get(v, "oidc_issuer"));
        const char *cid = yyjson_mut_get_str(yyjson_mut_obj_get(v, "oidc_client_id"));
        if (!exp || !ref || !iss || !cid) continue;
        int64_t at = iso8601_to_epoch(exp);
        if (at < 0 || now_ms() / 1000 < at - REFRESH_EARLY_S) continue;
        entry = v;
        break;
    }
    if (!entry) { yyjson_mut_doc_free(m); free(path); return; }

    /* Copies: the strings live in the doc we are about to mutate. */
    char *iss = xstrdup(yyjson_mut_get_str(yyjson_mut_obj_get(entry, "oidc_issuer")));
    char *cid = xstrdup(yyjson_mut_get_str(yyjson_mut_obj_get(entry, "oidc_client_id")));
    char *ref = xstrdup(yyjson_mut_get_str(yyjson_mut_obj_get(entry, "refresh_token")));

    buf_t form, body;
    buf_init(&form);
    buf_init(&body);
    form_append(&form, "grant_type", "refresh_token");
    form_append(&form, "refresh_token", ref);
    form_append(&form, "client_id", cid);
    char err[256] = "";
    int status = oauth_post_form(iss, "/oauth2/token", form.data, &body,
                                 err, sizeof err);
    buf_free(&form);
    memset(ref, 0, strlen(ref));
    free(ref);
    free(iss);
    free(cid);

    if (status >= 200 && status < 300) {
        yyjson_doc *tok = jparse(body.data, body.len);
        yyjson_val *troot = tok ? yyjson_doc_get_root(tok) : NULL;
        const char *access = jget_str(troot, "access_token");
        if (access && *access) {
            put_str(m, entry, "key", access);
            const char *nref = jget_str(troot, "refresh_token");
            if (nref && *nref) put_str(m, entry, "refresh_token", nref);
            int64_t now = now_ms() / 1000;
            char ts[32];
            iso8601_from_epoch(now, ts);
            put_str(m, entry, "create_time", ts);
            int64_t expires_in = jget_int(troot, "expires_in", 0);
            if (expires_in > 0) {
                iso8601_from_epoch(now + expires_in, ts);
                put_str(m, entry, "expires_at", ts);
            }
            store_save(m, path);
        }
        yyjson_doc_free(tok);
    } else if (tny_debug()) {
        fprintf(stderr, "tny: grok token refresh failed (HTTP %d): %s\n",
                status, err);
    }
    buf_free(&body);
    yyjson_mut_doc_free(m);
    free(path);
}

/* ---------- logout ---------- */

/* Drop the xAI entries tny knows how to mint (the legacy sign-in scope and
 * any entry pointing at the active issuer). Entries from other issuers —
 * enterprise IdPs configured in the grok CLI — are left alone; the file is
 * removed once it holds nothing else. */
int tny_grok_logout(void) {
    char *path = grok_auth_json_path();
    if (!path) return 1;
    if (!file_exists(path)) {
        printf("no grok session (~/.grok/auth.json missing)%s\n",
               getenv("XAI_API_KEY") ? "; unset XAI_API_KEY to drop the "
                                       "API-key fallback" : "");
        free(path);
        return 0;
    }
    yyjson_mut_doc *m = store_load(path);
    if (!m) { free(path); return 1; }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    const char *issuer = grok_issuer();
    size_t ilen = strlen(issuer);
    while (ilen && issuer[ilen - 1] == '/') ilen--;

    int removed = 0;
    for (;;) {
        yyjson_mut_val *victim = NULL;
        yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(root);
        yyjson_mut_val *k;
        while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
            const char *key = yyjson_mut_get_str(k);
            yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
            const char *iss =
                yyjson_mut_get_str(yyjson_mut_obj_get(v, "oidc_issuer"));
            bool ours = (key && strcmp(key, GROK_LEGACY_SCOPE) == 0) ||
                        (key && strncmp(key, issuer, ilen) == 0 &&
                         key[ilen] == ':' && key[ilen + 1] == ':') ||
                        (iss && strncmp(iss, issuer, ilen) == 0 &&
                         (iss[ilen] == 0 || iss[ilen] == '/'));
            if (ours) { victim = k; break; }
        }
        if (!victim) break;
        yyjson_mut_obj_remove_key(root, yyjson_mut_get_str(victim));
        removed++;
    }

    int rc = 0;
    if (removed == 0) {
        printf("no xAI session entries in ~/.grok/auth.json; nothing removed.\n");
    } else if (yyjson_mut_obj_size(root) == 0) {
        rc = unlink(path) == 0 ? 0 : 1;
        if (rc == 0) printf("grok session removed (~/.grok/auth.json deleted).\n");
        else fprintf(stderr, "tny: cannot remove %s\n", path);
    } else {
        rc = store_save(m, path) == 0 ? 0 : 1;
        if (rc == 0)
            printf("grok session removed from ~/.grok/auth.json "
                   "(other issuers kept).\n");
        else
            fprintf(stderr, "tny: cannot rewrite %s\n", path);
    }
    if (getenv("XAI_API_KEY"))
        printf("XAI_API_KEY is still set — unset it to drop the API-key "
               "fallback.\n");
    yyjson_mut_doc_free(m);
    free(path);
    return rc;
}
