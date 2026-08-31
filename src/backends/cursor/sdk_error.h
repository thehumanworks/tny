/* sdk_error.h -- bounded decoder for sdk.v1 Connect error details.
 *
 * Connect JSON carries SdkErrorDetails as an unpadded base64 protobuf Any.
 * Keeping this tiny decoder local avoids adding a protobuf runtime merely to
 * preserve the bridge's stable error taxonomy and retry metadata. */
#ifndef TNY_BACKENDS_CURSOR_SDK_ERROR_H
#define TNY_BACKENDS_CURSOR_SDK_ERROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CURSOR_SDK_MAX_ERROR_DETAIL (64u * 1024u)
#define CURSOR_SDK_MAX_ERROR_BODY   (8u * 1024u * 1024u)

typedef enum {
    CURSOR_SDK_ERR_UNSPECIFIED = 0,
    CURSOR_SDK_ERR_UNAUTHORIZED = 1,
    CURSOR_SDK_ERR_API_KEY_NOT_FOUND = 2,
    CURSOR_SDK_ERR_PLAN_REQUIRED = 3,
    CURSOR_SDK_ERR_ROLE_FORBIDDEN = 4,
    CURSOR_SDK_ERR_FEATURE_UNAVAILABLE = 5,
    CURSOR_SDK_ERR_AGENT_NOT_FOUND = 6,
    CURSOR_SDK_ERR_RUN_NOT_FOUND = 7,
    CURSOR_SDK_ERR_VALIDATION_ERROR = 8,
    CURSOR_SDK_ERR_INVALID_MODEL = 9,
    CURSOR_SDK_ERR_INVALID_BRANCH_NAME = 10,
    CURSOR_SDK_ERR_REPOSITORY_REQUIRED = 11,
    CURSOR_SDK_ERR_REPOSITORY_ACCESS = 12,
    CURSOR_SDK_ERR_PR_RESOLUTION_FAILED = 13,
    CURSOR_SDK_ERR_USAGE_LIMIT_EXCEEDED = 14,
    CURSOR_SDK_ERR_AGENT_BUSY = 15,
    CURSOR_SDK_ERR_AGENT_ARCHIVED = 16,
    CURSOR_SDK_ERR_RUN_NOT_CANCELLABLE = 17,
    CURSOR_SDK_ERR_RATE_LIMIT_EXCEEDED = 18,
    CURSOR_SDK_ERR_UPSTREAM_ERROR = 19,
    CURSOR_SDK_ERR_INTERNAL_ERROR = 20,
    CURSOR_SDK_ERR_CLIENT_CANCELLED = 21,
} cursor_sdk_error_code;

typedef struct {
    bool has_limit;
    bool has_remaining;
    bool has_reset_epoch_seconds;
    uint64_t limit;
    uint64_t remaining;
    uint64_t reset_epoch_seconds;
} cursor_sdk_rate_limit;

typedef struct {
    int http_status;
    char connect_code[40];
    int32_t sdk_error_code; /* retains future enum values */
    char *message;
    char *request_id;
    char *help_url;
    char *provider;
    bool has_sdk_details;
    bool has_retry_after;
    int64_t retry_after_seconds;
    int32_t retry_after_nanos;
    bool has_rate_limit;
    cursor_sdk_rate_limit rate_limit;
} cursor_sdk_error;

void cursor_sdk_error_init(cursor_sdk_error *error);
void cursor_sdk_error_free(cursor_sdk_error *error);

/* Parse a Connect JSON error body and its optional sdk.v1.SdkErrorDetails.
 * Unknown JSON/protobuf fields and future enum values are retained or ignored
 * according to the sdk.v1 additive compatibility contract. Returns 0 for a
 * valid Connect error object, -1 for malformed/non-object JSON. error must
 * first have been initialized with cursor_sdk_error_init(). */
int cursor_sdk_error_parse(cursor_sdk_error *error, const char *body, size_t len, int http_status);

/* Stable display spelling; future numeric values return "UNKNOWN". */
const char *cursor_sdk_error_code_name(int32_t code);

#endif
