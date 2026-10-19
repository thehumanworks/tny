/* errors.c — allocation-free native-provider failure policy. Classification,
 * bounded diagnostics and retry hints have no transport or turn-loop ownership. */
#include "backends/openai/errors.h"
#include "core/openai_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A provider error category reaches diagnostics as a bounded lowercase
 * identifier (invalid_request_error, rate_limit_exceeded, …) and nothing
 * else: message text may echo credentials or request content (ADR 0028), so
 * anything that is not a short identifier is dropped whole. */
void oa_error_token(char *out, size_t cap, const char *raw) {
    out[0] = 0;
    if (!raw || !*raw) return;
    size_t n = 0;
    for (const char *p = raw; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char keep;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') keep = (char)c;
        else if (c >= 'A' && c <= 'Z') keep = (char)(c + ('a' - 'A'));
        else if (c == '-' || c == '.' || c == ' ') keep = '_';
        else {
            out[0] = 0;
            return;
        }
        if (n + 1 >= cap) { /* too long for a category name */
            out[0] = 0;
            return;
        }
        out[n++] = keep;
    }
    out[n] = 0;
}

/* Opt-in only (TNY_DEBUG_PROVIDER_ERRORS=1): the provider's own message,
 * control characters blanked, bounded, cut on a UTF-8 boundary. */
static void set_error_detail(char *detail, size_t cap, bool debug, const char *msg) {
    detail[0] = 0;
    if (!debug || !msg) return;
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)msg;
    for (; *p && n + 1 < cap; p++) detail[n++] = (*p < 0x20 || *p == 0x7f) ? ' ' : (char)*p;
    if (*p) { /* truncated: never end inside a multi-byte sequence */
        while (n > 0 && (detail[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0 && (detail[n - 1] & 0x80)) n--;
    }
    detail[n] = 0;
}

bool oa_error_token_is_permanent(const char *token) {
    static const char *const permanent[] = {"invalid",         "context_length", "insufficient",
                                            "model_not_found", "not_found",      "unsupported",
                                            "unknown_model",   "billing"};
    for (size_t i = 0; i < sizeof permanent / sizeof permanent[0]; i++)
        if (str_starts(token, permanent[i])) return true;
    return false;
}

bool oa_status_is_retryable(int status) {
    return status == 408 || status == 409 || status == 425 || status == 429 || status >= 500;
}

static const struct {
    const char *code;
    oa_plan_error error;
} oa_plan_codes[] = {
    {"subscription_sharing_usage_limit_exceeded", OA_PLAN_ERROR_USAGE_LIMIT},
    {"subscription_sharing_usage_unavailable", OA_PLAN_ERROR_UNAVAILABLE},
    {"subscription_sharing_user_unavailable", OA_PLAN_ERROR_UNAVAILABLE},
    {"subscription_sharing_user_not_eligible", OA_PLAN_ERROR_NOT_ELIGIBLE},
    {"subscription_sharing_route_not_supported", OA_PLAN_ERROR_ROUTE},
    {"chatpass_v2_scope_not_authorized", OA_PLAN_ERROR_GRANT},
    {"chatpass_v2_invalid_authorization_context", OA_PLAN_ERROR_GRANT},
    {"subscription_sharing_invalid_user", OA_PLAN_ERROR_INVALID_USER},
    {"subscription_sharing_unsupported_capability", OA_PLAN_ERROR_UNSUPPORTED},
};

oa_plan_error oa_plan_error_of(const char *code) {
    if (!code) return OA_PLAN_ERROR_NONE;
    for (size_t i = 0; i < sizeof oa_plan_codes / sizeof oa_plan_codes[0]; i++)
        if (strcmp(code, oa_plan_codes[i].code) == 0) return oa_plan_codes[i].error;
    return OA_PLAN_ERROR_NONE;
}

/* The table's own copy of a plan code, so a diagnostic never carries
 * provider bytes; NULL for anything else. */
static const char *plan_code_static(const char *code) {
    if (!code) return NULL;
    for (size_t i = 0; i < sizeof oa_plan_codes / sizeof oa_plan_codes[0]; i++)
        if (strcmp(code, oa_plan_codes[i].code) == 0) return oa_plan_codes[i].code;
    return NULL;
}

bool oa_plan_error_retryable(oa_plan_error e) { return e == OA_PLAN_ERROR_UNAVAILABLE; }

void oa_error_param(char *out, size_t cap, const char *raw) {
    if (!out || cap == 0) return;
    out[0] = 0;
    size_t n = raw ? strlen(raw) : 0;
    if (n == 0 || n >= cap) return;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)raw[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '[' || c == ']' || c == '-';
        if (!ok) return;
    }
    memcpy(out, raw, n + 1);
}

/* Classify one error payload: the `error` member (object or string) of an
 * HTTP error body or SSE event, or a Responses `error` event itself. */
void oa_error_classify(yyjson_val *err, int http_status, bool debug, char *detail,
                       size_t detail_cap, oa_error_info *info) {
    memset(info, 0, sizeof *info);
    info->status = http_status;
    if (err && yyjson_is_obj(err)) {
        const char *type = jget_str(err, "type");
        const char *code = jget_str(err, "code");
        oa_error_token(info->token, sizeof info->token, type ? type : code);
        if (!info->token[0] && type && code) oa_error_token(info->token, sizeof info->token, code);
        yyjson_val *cv = jget(err, "code");
        if (cv && yyjson_is_int(cv)) info->code = (int)yyjson_get_int(cv);
        if (!info->code) info->code = (int)jget_int(err, "status", 0);
        info->plan_code = plan_code_static(code ? code : type);
        info->plan = oa_plan_error_of(info->plan_code);
        if (info->plan == OA_PLAN_ERROR_UNSUPPORTED)
            oa_error_param(info->param, sizeof info->param, jget_str(err, "param"));
        set_error_detail(detail, detail_cap, debug, jget_str(err, "message"));
    } else if (err && yyjson_is_str(err)) {
        set_error_detail(detail, detail_cap, debug, yyjson_get_str(err));
    }
    if (info->plan) info->retryable = oa_plan_error_retryable(info->plan);
    else if (info->status >= 400) info->retryable = oa_status_is_retryable(info->status);
    else if (info->code >= 400) info->retryable = oa_status_is_retryable(info->code);
    else info->retryable = !oa_error_token_is_permanent(info->token);
}

/* The recovery line for a ChatGPT-plan code (docs/adr/0186), which can
 * arrive behind any status or in-stream; `st` is the status annotation. */
static void plan_error_text(const oa_error_info *info, const char *st, char *out, size_t cap) {
    const char *code = info->plan_code ? info->plan_code : "";
    switch (info->plan) {
    case OA_PLAN_ERROR_USAGE_LIMIT:
        snprintf(out, cap,
                 "ChatGPT plan usage limit reached (%s%s): tny paused requests on your plan; "
                 "check usage at %s",
                 st, code, TNY_CHATGPT_USAGE_URL);
        break;
    case OA_PLAN_ERROR_UNAVAILABLE:
        snprintf(out, cap, "ChatGPT plan usage is temporarily unavailable (%s%s)", st, code);
        break;
    case OA_PLAN_ERROR_NOT_ELIGIBLE:
        snprintf(out, cap,
                 "ChatGPT plan usage is not available for this account, workspace or policy "
                 "(%s%s); signing in again will not change it — use an OpenAI API key instead",
                 st, code);
        break;
    case OA_PLAN_ERROR_ROUTE:
        snprintf(out, cap, "the ChatGPT plan does not serve this endpoint (%s%s)", st, code);
        break;
    case OA_PLAN_ERROR_GRANT:
        snprintf(out, cap,
                 "the ChatGPT sign-in does not authorize this request (%s%s): sign in again "
                 "with `tny --provider openai login` and allow ChatGPT plan use",
                 st, code);
        break;
    case OA_PLAN_ERROR_INVALID_USER:
        snprintf(out, cap,
                 "ChatGPT could not validate the signed-in user (%s%s): sign in again with "
                 "`tny --provider openai login`",
                 st, code);
        break;
    case OA_PLAN_ERROR_UNSUPPORTED:
        if (info->param[0])
            snprintf(out, cap, "the ChatGPT plan does not support `%s` in this request (%s%s)",
                     info->param, st, code);
        else
            snprintf(out, cap, "the ChatGPT plan does not support part of this request (%s%s)", st,
                     code);
        break;
    case OA_PLAN_ERROR_NONE: out[0] = 0; break;
    }
}

/* The user-facing line for a failure; detail is opt-in and final-only. */
void oa_error_format(const oa_error_info *info, bool plan_route, bool grok_proxy,
                     const char *detail, char *out, size_t cap) {
    char cat[48];
    snprintf(cat, sizeof cat, "%s%s", info->token[0] ? ", " : "", info->token);
    char st[24] = "";
    if (info->status) snprintf(st, sizeof st, "HTTP %d, ", info->status);
    if (info->plan) plan_error_text(info, st, out, cap);
    else if (info->status == 426 && grok_proxy)
        snprintf(out, cap,
                 "Grok requires a newer client version (HTTP 426%s): update tny or set "
                 "TNY_GROK_CLIENT_VERSION to a supported Grok Build version",
                 cat);
    else if (info->status == 401 && plan_route)
        snprintf(out, cap,
                 "ChatGPT sign-in was not accepted (HTTP 401%s): sign in again with "
                 "`tny --provider openai login`",
                 cat);
    else if (info->status == 401)
        snprintf(out, cap, "authentication failed (HTTP 401%s): check the API key", cat);
    else if (info->status == 403 && plan_route)
        snprintf(out, cap,
                 "ChatGPT plan request refused (HTTP 403%s): a policy or permission check, "
                 "such as the serving region, blocked it",
                 cat);
    else if (info->status == 403)
        snprintf(out, cap,
                 "provider refused the request (HTTP 403%s): the key may lack access to "
                 "this model, or a gateway blocked the request",
                 cat);
    else if (info->status == 429) snprintf(out, cap, "provider rate limit (HTTP 429%s)", cat);
    else if (info->status >= 500)
        snprintf(out, cap, "provider error (HTTP %d%s)", info->status, cat);
    else if (info->status >= 400)
        snprintf(out, cap, "provider rejected the request (HTTP %d%s)", info->status, cat);
    else if (info->code >= 400)
        snprintf(out, cap, "provider stream reported an error (code %d%s)", info->code, cat);
    else snprintf(out, cap, "provider stream reported an error%s", cat);
    if (detail && detail[0]) {
        size_t n = strlen(out);
        if (n < cap) snprintf(out + n, cap - n, ": %s", detail);
    }
}

int oa_error_retry_after_ms(const char *value) {
    if (!value) return 0;
    while (*value == ' ') value++;
    if (*value < '0' || *value > '9') return 0; /* an HTTP-date: use the backoff */
    long secs = strtol(value, NULL, 10);
    if (secs <= 0) return 0;
    if (secs > OA_RETRY_MAX_MS / 1000) secs = OA_RETRY_MAX_MS / 1000;
    return (int)secs * 1000;
}
