/* Private native-provider ownership. No public ABI; C retains scheduling.
 * Handles must not be copied. Borrowed views are synchronous and must not be
 * freed. Request storage is independent of connection reset/cancellation. */
#ifndef TNY_OPENAI_REQUEST_OWNER_H
#define TNY_OPENAI_REQUEST_OWNER_H

#include "json/json.h"
#include "net/net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oa_request_owner oa_request_owner;
typedef struct oa_connection_owner oa_connection_owner;

typedef struct {
    const char *prefix, *endpoint;
    const char *auth_name, *auth_prefix, *api_key;
    const char *provider_name, *base_url, *session_id;
    char *const *extra_headers;
    /* Stack/config borrows, valid through both synchronous sends. */
    const char *session_header, *thread_header, *state_header;
} oa_request_options;

oa_request_owner *oa_request_new(void);
/* Inline builder workspace. buf_* mutates the borrowed value; reset and detach
 * are explicit ownership operations. No buffer gets a separate heap handle. */
typedef enum {
    OA_BUILD_BODY,
    OA_BUILD_SYSTEM,
    OA_BUILD_IMAGE,
    OA_BUILD_BUFFER_COUNT
} oa_build_buffer;
typedef enum {
    OA_BUILD_MESSAGE,
    OA_BUILD_INPUT,
    OA_BUILD_SCHEMA,
    OA_BUILD_FLAT,
    OA_BUILD_FORMAT,
    OA_BUILD_STRING_COUNT
} oa_build_string;
buf_t *oa_request_buffer(oa_request_owner *request, oa_build_buffer which);
const char *oa_request_take_string(oa_request_owner *request, oa_build_string which, char *owned);
/* Consume the document unconditionally; the returned view expires on the next
 * take, preparation or request destruction. No allocation and no failure. */
yyjson_mut_doc *oa_request_take_view(oa_request_owner *request, yyjson_mut_doc *view);
/* All handle arguments are non-NULL. Consume body even on failure. Prepare
 * exactly once per request (including failed attempts); failure leaves
 * no sendable request. Returns 0 or -2 (OOM), never a retryable I/O status. */
int oa_request_prepare(oa_request_owner *request, char *body, const oa_request_options *options);
void oa_request_free(oa_request_owner **request);

oa_connection_owner *oa_connection_new(void);
/* Explicit replacement closes the old connection before opening the new one.
 * Request bytes/header borrows are unaffected. 0 success, -1 I/O, -2 OOM. */
int oa_connection_open(oa_connection_owner *owner, const char *url, char *err, size_t errlen);
http_conn *oa_connection_get(const oa_connection_owner *owner);
void oa_connection_drop(oa_connection_owner *owner);
void oa_connection_free(oa_connection_owner **owner);
/* Synchronous HTTP write; no callbacks or policy decisions. */
int oa_request_send(oa_request_owner *request, oa_connection_owner *connection);

#ifdef __cplusplus
}
#endif
#endif
