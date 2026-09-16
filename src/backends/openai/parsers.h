/* Private C facade. Views are borrowed; only these functions mutate owners. */
#ifndef TNY_OPENAI_PARSERS_H
#define TNY_OPENAI_PARSERS_H
#ifdef __cplusplus
extern "C" {
#endif
#include "json/json.h"
/* ---- streamed tool_call assembly (src/backends/openai/toolcalls.cpp) ----
 * Chat Completions streams tool calls as fragment deltas. Well-behaved
 * providers key every fragment by "index"; gateways have been observed
 * repeating or omitting the index while carrying a fresh "id" per call
 * (a lost call there poisons the transcript: the provider 400s the next
 * request with "no tool output found for function call …"). Attribution
 * is therefore id-first; exposed for unit tests (tests/test_openai.c). */
#define OA_MAX_TOOL_CALLS 32

typedef struct {
    char *id;       /* provider call id; NULL until (if ever) streamed */
    char *name;     /* function name; NULL until streamed */
    buf_t args;     /* read-only view of owned fragments; never buf_free/mutate */
    int wire_index; /* provider "index" for this call; -1 if never sent */
} oa_call;

typedef struct {
    oa_call calls[OA_MAX_TOOL_CALLS];
    int n;
    void *owner; /* C++ storage; calls[] contains borrowed read-only views */
    int status;  /* sticky -2 on OOM until reset */
} oa_callset;

/* Merge one streamed `delta.tool_calls` array into the set. Fragments are
 * attributed by id when present (new id = new call), else by wire index,
 * else to the most recent call. Fragments beyond OA_MAX_TOOL_CALLS or with
 * a negative/out-of-range index are dropped. Returns 0 or sticky -2 (OOM).
 * Zero-initialize a new set; never copy an initialized facade. */
int oa_calls_feed(oa_callset *cs, yyjson_val *tool_calls);
void oa_calls_reset(oa_callset *cs);
/* The id sent upstream: the provider's id, or a slot-unique fallback
 * (never a shared constant — duplicate ids also unpair the transcript).
 * Writes into buf (>= 16 bytes) only when the fallback is needed. */
const char *oa_call_id(const oa_call *pc, int slot, char *buf, size_t buflen);

/* Set one slot, retaining owned copies. Existing id/name win unless absent.
 * replace_args is used for authoritative Responses done/restore records. */
int oa_calls_set(oa_callset *cs, int slot, int wire_index, const char *id, const char *name,
                 const char *args, bool replace_args);

/* Event views belong to the decoder document and expire when emit returns.
 * C callbacks must not throw. Retained JSON must be deep copied by the sink. */
/* C11 enum layout is shared with C callers; a C++ fixed base would change it. */
// NOLINTNEXTLINE(performance-enum-size)
typedef enum {
    OA_DECODE_DONE,
    OA_DECODE_ERROR,
    OA_DECODE_USAGE_CHAT,
    OA_DECODE_USAGE_RSP,
    OA_DECODE_FINISH,
    OA_DECODE_TEXT,
    OA_DECODE_REASONING_CONTENT,
    OA_DECODE_THINKING,
    OA_DECODE_RSP_THINKING,
    OA_DECODE_DETAILS,
    OA_DECODE_REASONING_ITEM,
    OA_DECODE_HOSTED_ITEM,
    OA_DECODE_HOSTED_OUTPUT,
    OA_DECODE_INCOMPLETE
} oa_decoded_kind;
typedef void (*oa_decoded_cb)(oa_decoded_kind kind, yyjson_val *value, const char *bytes,
                              size_t len, void *ud);
/* 0 decoded/ignored, 1 malformed JSON (ignored by stream policy), -2 OOM. */
int oa_decode_event(bool chat, const char *bytes, size_t len, oa_callset *calls, oa_decoded_cb emit,
                    void *ud);

#ifdef __cplusplus
}
#endif
#endif
