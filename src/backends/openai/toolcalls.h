/* Narrow private tool-fragment facade shared by C scheduling and C++ decoding. */
#ifndef TNY_OPENAI_TOOLCALLS_H
#define TNY_OPENAI_TOOLCALLS_H
#include "json/json.h"
#ifdef __cplusplus
extern "C" {
#endif
/* ---- streamed tool_call assembly (src/backends/openai/toolcalls.cpp) ----
 * Chat Completions streams tool calls as fragment deltas. Well-behaved
 * providers key every fragment by "index"; gateways have been observed
 * repeating or omitting the index while carrying a fresh "id" per call
 * (a lost call there poisons the transcript: the provider 400s the next
 * request with "no tool output found for function call …"). Attribution
 * is therefore id-first; exposed for unit tests (tests/test_openai.c). */
#define OA_MAX_TOOL_CALLS 32

typedef struct {
    const char *id;   /* borrowed until next callset mutation/reset */
    const char *name; /* borrowed, NULL until streamed */
    struct {
        const char *data;
        size_t len;
    } args;         /* borrowed concatenated argument fragments */
    int wire_index; /* provider "index" for this call; -1 if never sent */
} oa_call;

typedef struct {
    oa_call calls[OA_MAX_TOOL_CALLS];
    int n;
    void *owner; /* owns every pointer above; zero initialize, do not copy */
    int status;  /* sticky allocation failure; reset before reuse */
} oa_callset;

/* Merge one streamed `delta.tool_calls` array into the set. Fragments are
 * attributed by id when present (new id = new call), else by wire index,
 * else to the most recent call. Fragments beyond OA_MAX_TOOL_CALLS or with
 * a negative index are dropped. */
int oa_calls_feed(oa_callset *cs, yyjson_val *tool_calls);
void oa_calls_reset(oa_callset *cs);
/* Own checkpoint records verbatim, preserving slot order and missing indices. */
int oa_calls_restore(oa_callset *cs, yyjson_val *records);
/* Responses/checkpoint ingress. Copy input strings before returning. A negative
 * or out-of-range index is ignored. replace_args is authoritative item.done;
 * append_only updates an existing index without creating an orphan. */
int oa_calls_item(oa_callset *cs, int64_t index, const char *id, const char *name, const char *args,
                  bool replace_args, bool append_only);
/* The id sent upstream: the provider's id, or a slot-unique fallback
 * (never a shared constant — duplicate ids also unpair the transcript).
 * Writes into buf (>= 16 bytes) only when the fallback is needed. */
const char *oa_call_id(const oa_call *pc, int slot, char *buf, size_t buflen);

int oa_reasoning_details_merge(yyjson_mut_doc *rdoc, yyjson_mut_val *arr, yyjson_val *details);
#ifdef __cplusplus
}
#endif
#endif
