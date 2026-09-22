/* tnyjev — isolated, typed TypeSafe Jev decision client (C11, C/C++ ABI).
 * No runtime, sessions, settings, environment lookup or chat-provider dependency.
 * See docs/tnyjev.md for the wire contract, limits and wasm behavior. */
#ifndef TNYJEV_H
#define TNYJEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TNYJEV_MAX_CHOICES   255u
#define TNYJEV_MAX_BYTES     (1024u * 1024u)
#define TNYJEV_DEFAULT_URL   "https://api.typesafe.ai/v1/systemone"
#define TNYJEV_DEFAULT_MODEL "jev-latest"

/* JSON values must encode a string, object or array (also null for a choice
 * description). TEXT is escaped, never interpreted as JSON. All strings are
 * borrowed, NUL-terminated UTF-8. NULL description.data means JSON null. */
typedef enum { TNYJEV_TEXT, TNYJEV_JSON } tnyjev_value_type;
typedef struct {
    tnyjev_value_type type;
    const char *data;
} tnyjev_value;
typedef struct {
    const char *key;
    tnyjev_value description;
} tnyjev_choice;

typedef enum { TNYJEV_SCORE, TNYJEV_CHOOSE } tnyjev_kind;
typedef struct {
    tnyjev_kind kind;
    const char *instructions;
    tnyjev_value state;
    const tnyjev_choice *choices; /* CHOOSE only; unique, nonempty keys */
    size_t choice_count;          /* CHOOSE: 1..255; SCORE: zero */
} tnyjev_request;

typedef struct {
    const char *api_key;               /* required; never logged or retained */
    const char *url;                   /* NULL: default; HTTPS or loopback HTTP only */
    const char *model;                 /* NULL: default */
    unsigned timeout_ms;               /* 0: 60000; response budget, maximum 300000 */
    bool (*cancelled)(void *userdata); /* optional; called on this thread */
    void *userdata;
} tnyjev_config;

typedef enum {
    TNYJEV_OK,
    TNYJEV_INVALID,
    TNYJEV_AUTH,
    TNYJEV_TRANSPORT,
    TNYJEV_HTTP,
    TNYJEV_PROTOCOL,
    TNYJEV_TIMEOUT,
    TNYJEV_CANCELLED,
    TNYJEV_OOM
} tnyjev_status;

/* No owned pointers: results can be copied, have no destructor, and do not
 * retain request storage. choice_index/probabilities follow request order.
 * SCORE is Jev's Noul P(yes), NOT its ordinal rubric Score primitive. */
typedef struct {
    tnyjev_kind kind;
    char model[256];
    uint64_t input_tokens;
    uint64_t output_tokens;
    union {
        double score;
        struct {
            size_t choice_index;
            size_t count;
            double probabilities[TNYJEV_MAX_CHOICES];
            double confidence;
        } choose;
    } value;
} tnyjev_result;

/* One synchronous call, no retries or redirects. Drives the shared transport
 * with tny_poll, never raw poll. Connect/write use shared transport deadlines;
 * timeout_ms bounds the response. Cancellation is observed between transport
 * operations. On failure *result is zeroed; err (when non-NULL, errlen > 0)
 * gets a secret-safe diagnostic. Caller owns all input storage for this call.
 * No live API requests occur until this function is explicitly invoked. */
tnyjev_status tnyjev_evaluate(const tnyjev_config *config, const tnyjev_request *request,
                              tnyjev_result *result, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
