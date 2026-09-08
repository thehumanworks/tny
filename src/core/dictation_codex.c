/* ChatGPT dictation contract: Codex rust-v0.105.0 tui/src/voice.rs,
 * transcribe_bytes. Account auth + WAV multipart, independent of chat config. */
#include "core/dictation_provider.h"
#include "json/json.h"
#include "net/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DICTATION_WIRE_MAX (TNY_DICTATION_TEXT_MAX * 6u + 1024u)
typedef struct {
    http_conn *conn;
    buf_t response;
    bool headers;
    int64_t deadline;
} codex_dictation;

static bool available(const tny_ctx *ctx, char *err, size_t len) {
    tny_codex_creds c;
    tny_codex_credentials(ctx, &c);
    bool ok = c.access_token && *c.access_token && c.account_id && *c.account_id &&
              !strpbrk(c.access_token, "\r\n") && !strpbrk(c.account_id, "\r\n");
    tny_codex_creds_free(&c);
    snprintf(err, len, "%s",
             ok ? "" : "dictation requires a ChatGPT login: tny --provider codex login");
    return ok;
}

static void destroy(void *job) {
    codex_dictation *d = job;
    if (!d) return;
    http_close(d->conn);
    if (d->response.data) secure_zero(d->response.data, d->response.len);
    buf_free(&d->response);
    free(d);
}

static void *start(const tny_ctx *ctx, const buf_t *wav, char *err, size_t len) {
    if (!(ctx && ctx->chatgpt_token && *ctx->chatgpt_token)) tny_codex_refresh_if_stale();
    if (!available(ctx, err, len)) return NULL;
    tny_codex_creds creds;
    tny_codex_credentials(ctx, &creds);
    buf_t url = {0}, auth = {0}, account = {0}, body = {0};
    codex_dictation *d = calloc(1, sizeof *d);
    bool ok = false;
    if (!d || !creds.access_token || !creds.account_id || strpbrk(creds.access_token, "\r\n") ||
        strpbrk(creds.account_id, "\r\n"))
        goto done;
    /* Only the trusted Codex gateway override can redirect this bearer. */
    const char *base = getenv("TNY_CODEX_BASE_URL");
    if (!base || !*base) base = "https://chatgpt.com/backend-api/codex";
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    if (n >= 6 && !memcmp(base + n - 6, "/codex", 6)) n -= 6;
    buf_append(&url, base, n);
    buf_appends(&url, "/transcribe");
    buf_appendf(&auth, "Authorization: Bearer %s", creds.access_token);
    buf_appendf(&account, "ChatGPT-Account-Id: %s", creds.account_id);
    /* Pick a boundary absent from the binary payload, including crafted WAVs. */
    char boundary[64] = "tny-dictation-";
    uint8_t random[16];
    if (!random_bytes(random, sizeof random)) goto done;
    for (size_t i = 0; i < sizeof random; i++)
        snprintf(boundary + 14 + i * 2, sizeof boundary - 14 - i * 2, "%02x", random[i]);
    size_t bn = strlen(boundary);
    for (size_t i = 0; i + bn <= wav->len; i++) {
        if (wav->data[i] == 't' && !memcmp(wav->data + i, boundary, bn)) {
            snprintf(err, len, "audio contains multipart boundary; retry dictation");
            goto done;
        }
    }
    buf_appendf(&body,
                "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                "Content-Type: audio/wav\r\n\r\n",
                boundary);
    buf_append(&body, wav->data, wav->len);
    buf_appendf(&body, "\r\n--%s--\r\n", boundary);
    char content_type[128];
    snprintf(content_type, sizeof content_type, "Content-Type: multipart/form-data; boundary=%s",
             boundary);
    static const char user_agent[] = "User-Agent: tny/" TNY_VERSION;
    const char *headers[] = {
        auth.data,  account.data,      content_type, "Accept: application/json",
        user_agent, "originator: tny", NULL};
    if (url.oom || auth.oom || account.oom || body.oom) goto done;
    d->deadline = monotonic_ms() + 60000;
    d->conn = http_open(url.data, err, len);
    if (!d->conn ||
        http_request(d->conn, "POST", http_prefix(d->conn), headers, body.data, body.len))
        goto done;
    ok = true;
done:
    if (auth.data) secure_zero(auth.data, auth.len);
    if (body.data) secure_zero(body.data, body.len);
    buf_free(&url);
    buf_free(&auth);
    buf_free(&account);
    buf_free(&body);
    tny_codex_creds_free(&creds);
    if (!ok) {
        destroy(d);
        if (!*err) snprintf(err, len, "cannot start dictation request");
        return NULL;
    }
    return d;
}

static int fd(const void *job) { return http_fd(((const codex_dictation *)job)->conn); }

static int step(void *job, buf_t *text, char *err, size_t len) {
    codex_dictation *d = job;
    if (monotonic_ms() >= d->deadline) {
        snprintf(err, len, "dictation request timed out");
        return 1;
    }
    if (!d->headers) {
        int status = http_read_response(d->conn, 0);
        if (status == -2) return -1;
        if (status != 200) {
            /* Never print a provider body: it may contain credentials/audio. */
            snprintf(err, len, "dictation request failed (HTTP %d)%s", status,
                     status == 401 || status == 403 ? "; run tny --provider codex login" : "");
            return status > 0 ? 2 : 1;
        }
        const char *ct = http_header(d->conn, "Content-Type");
        if (!ct || !str_starts(ct, "application/json")) {
            snprintf(err, len, "unexpected dictation response content type");
            return 1;
        }
        d->headers = true;
    }
    for (int i = 0; i < 16; i++) {
        char chunk[8192];
        ssize_t n = http_body_read(d->conn, chunk, sizeof chunk);
        if (n == -2) return -1;
        if (n < 0) {
            snprintf(err, len, "incomplete dictation response");
            return 1;
        }
        if (!n) {
            yyjson_doc *doc = jparse(d->response.data, d->response.len);
            yyjson_val *value = doc ? jget(yyjson_doc_get_root(doc), "text") : NULL;
            const char *s = yyjson_get_str(value);
            size_t bytes = yyjson_get_len(value);
            bool valid = s && bytes <= TNY_DICTATION_TEXT_MAX;
            if (valid) buf_append(text, s, bytes);
            yyjson_doc_free(doc);
            if (!valid || text->oom) {
                snprintf(err, len, "invalid or oversized dictation transcript");
                return 1;
            }
            return 0;
        }
        if ((size_t)n > DICTATION_WIRE_MAX - d->response.len) {
            snprintf(err, len, "dictation response exceeds wire limit");
            return 1;
        }
        buf_append(&d->response, chunk, (size_t)n);
        if (d->response.oom) {
            snprintf(err, len, "out of memory");
            return 1;
        }
    }
    return -1;
}

const tny_dictation_provider tny_dictation_codex = {"codex", available, start, fd, step, destroy};
