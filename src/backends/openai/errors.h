/* errors.h — private provider failure classification and diagnostics.
 * No public ABI, network I/O, retained JSON borrows or provider-state access. */
#ifndef TNY_OPENAI_ERRORS_H
#define TNY_OPENAI_ERRORS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "json/json.h"

/* Shared cap for Retry-After and the loop's exponential backoff. */
#define OA_RETRY_MAX_MS 30000

/* A provider error category as a bounded lowercase identifier
 * ([a-z0-9_], at most cap-1 bytes; '-', '.', ' ' fold to '_'). Anything
 * else — free text, over-long strings — yields "" so message text never
 * reaches a diagnostic. */
void oa_error_token(char *out, size_t cap, const char *raw);
/* Categories that a retry cannot fix (invalid_*, context_length_*, …). */
bool oa_error_token_is_permanent(const char *token);
/* HTTP statuses worth a bounded retry: 408, 409, 425, 429, and 5xx. */
bool oa_status_is_retryable(int status);

/* ChatGPT-plan error codes with their own recovery (Sign in with ChatGPT,
 * docs/adr/0186). They arrive as `error.code` on an HTTP error body or a
 * response.failed event, whatever the status. */
typedef enum {
    OA_PLAN_ERROR_NONE = 0,
    OA_PLAN_ERROR_USAGE_LIMIT,  /* pause; link to ChatGPT usage settings */
    OA_PLAN_ERROR_UNAVAILABLE,  /* usage/user check unavailable: retry */
    OA_PLAN_ERROR_NOT_ELIGIBLE, /* user, workspace or policy excluded */
    OA_PLAN_ERROR_ROUTE,        /* method/endpoint not on the plan route */
    OA_PLAN_ERROR_GRANT,        /* signed permission context refused */
    OA_PLAN_ERROR_INVALID_USER, /* subscriber context invalid: sign in again */
    OA_PLAN_ERROR_UNSUPPORTED   /* an input named by error.param */
} oa_plan_error;
/* The plan error for an exact provider code; NONE for anything else. */
oa_plan_error oa_plan_error_of(const char *code);
/* Only the "temporarily unavailable" plan errors are worth a retry. */
bool oa_plan_error_retryable(oa_plan_error e);
/* `error.param` as a bounded field path ([A-Za-z0-9_.[]-], at most cap-1
 * bytes); anything else yields "". */
void oa_error_param(char *out, size_t cap, const char *raw);

/* One classified provider failure (docs/adr/0069): the HTTP status (0 for
 * an in-stream error event), a bounded category token, whether a retry can
 * reasonably succeed, and the provider's Retry-After when it sent one. */
typedef struct {
    int status; /* HTTP status; 0 for an in-stream error event */
    int code;   /* numeric code inside the payload (gateways relay upstream statuses) */
    char token[33];
    bool retryable;
    int retry_after_ms;
    oa_plan_error plan;    /* a ChatGPT-plan code (docs/adr/0186) */
    const char *plan_code; /* its static spelling, or NULL */
    char param[64];        /* error.param, when it is a field path */
} oa_error_info;

/* err is the error object/string, not its containing HTTP/SSE envelope.
 * detail is caller-owned storage with positive capacity. An object/string
 * replaces it (empty unless debug); an absent payload leaves it unchanged.
 * info owns its bounded fields; plan_code points into a static allowlist. */
void oa_error_classify(yyjson_val *err, int http_status, bool debug, char *detail,
                       size_t detail_cap, oa_error_info *info);
/* Supply detail only for a final diagnostic after explicit debug opt-in.
 * grok_proxy means the Grok profile carries its subscription proxy header.
 * out has positive capacity. No provider-supplied message is used otherwise. */
void oa_error_format(const oa_error_info *info, bool plan_route, bool grok_proxy,
                     const char *detail, char *out, size_t cap);
/* Delta-seconds only, capped at OA_RETRY_MAX_MS; HTTP dates use loop backoff. */
int oa_error_retry_after_ms(const char *value);

#ifdef __cplusplus
}
#endif

#endif
