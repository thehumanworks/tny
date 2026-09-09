/* Bounded WAV multipart and incremental JSON response shared by STT adapters. */
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
} dictation_http;

void tny_dictation_http_destroy(void *job) {
    dictation_http *d = job;
    if (!d) return;
    http_close(d->conn);
    if (d->response.data) secure_zero(d->response.data, d->response.len);
    buf_free(&d->response);
    free(d);
}

void *tny_dictation_http_start(const char *url, const char *token, const char *account_id,
                               const buf_t *wav, char *err, size_t len) {
    buf_t auth = {0}, account = {0}, body = {0};
    dictation_http *d = calloc(1, sizeof *d);
    bool ok = false;
    if (!d) goto done;
    buf_appendf(&auth, "Authorization: Bearer %s", token);
    if (account_id) buf_appendf(&account, "ChatGPT-Account-Id: %s", account_id);
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
    const char *headers[] = {auth.data, content_type, "Accept: application/json", user_agent, NULL,
                             NULL,      NULL};
    if (account_id) {
        headers[4] = account.data;
        headers[5] = "originator: tny";
    }
    if (auth.oom || account.oom || body.oom) goto done;
    d->deadline = monotonic_ms() + 60000;
    /* Transport diagnostics can echo URLs; keep this boundary secret-safe. */
    char transport_error[256] = "";
    d->conn = http_open(url, transport_error, sizeof transport_error);
    if (!d->conn ||
        http_request(d->conn, "POST", http_prefix(d->conn), headers, body.data, body.len))
        goto done;
    ok = true;
done:
    if (auth.data) secure_zero(auth.data, auth.len);
    if (body.data) secure_zero(body.data, body.len);
    buf_free(&auth);
    buf_free(&account);
    buf_free(&body);
    if (!ok) {
        tny_dictation_http_destroy(d);
        if (!*err) snprintf(err, len, "cannot start dictation request");
        return NULL;
    }
    return d;
}

int tny_dictation_http_fd(const void *job) { return http_fd(((const dictation_http *)job)->conn); }

int tny_dictation_http_step(void *job, buf_t *text, char *err, size_t len) {
    dictation_http *d = job;
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
                     status == 401 || status == 403 ? "; check STT credentials and entitlement"
                                                    : "");
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
