/* openai.h — native backend extras: the frontend binds the session,
 * permission engine, and approval hook before send(). */
#ifndef TNY_OPENAI_H
#define TNY_OPENAI_H

#include "core/backend.h"
#include "core/session.h"
#include "core/perm.h"
#include "json/json.h"

void tny_backend_openai_bind(tny_backend *b, tny_session_state *session,
                             perm_engine *perm,
                             tny_perm_decision (*prompt)(const char *tool,
                                                         const char *summary,
                                                         void *ud),
                             void *prompt_ud);

/* Number of agent steps taken in the last turn + tool call log (JSON array
 * text, borrowed until next send). */
int tny_backend_openai_steps(tny_backend *b);
const char *tny_backend_openai_toolcalls_json(tny_backend *b);

/* ---- streamed tool_call assembly (src/backends/openai/toolcalls.c) ----
 * Chat Completions streams tool calls as fragment deltas. Well-behaved
 * providers key every fragment by "index"; gateways have been observed
 * repeating or omitting the index while carrying a fresh "id" per call
 * (a lost call there poisons the transcript: the provider 400s the next
 * request with "no tool output found for function call …"). Attribution
 * is therefore id-first; exposed for unit tests (tests/test_openai.c). */
#define OA_MAX_TOOL_CALLS 32

typedef struct {
    char *id;        /* provider call id; NULL until (if ever) streamed */
    char *name;      /* function name; NULL until streamed */
    buf_t args;      /* concatenated argument fragments */
    int   wire_index;/* provider "index" for this call; -1 if never sent */
} oa_call;

typedef struct {
    oa_call calls[OA_MAX_TOOL_CALLS];
    int n;
} oa_callset;

/* Merge one streamed `delta.tool_calls` array into the set. Fragments are
 * attributed by id when present (new id = new call), else by wire index,
 * else to the most recent call. Fragments beyond OA_MAX_TOOL_CALLS or with
 * a negative index are dropped. */
void oa_calls_feed(oa_callset *cs, yyjson_val *tool_calls);
void oa_calls_reset(oa_callset *cs);
/* The id sent upstream: the provider's id, or a slot-unique fallback
 * (never a shared constant — duplicate ids also unpair the transcript).
 * Writes into buf (>= 16 bytes) only when the fallback is needed. */
const char *oa_call_id(const oa_call *pc, int slot, char *buf, size_t buflen);

/* Normalize a user-supplied JSON Schema into a Chat Completions
 * `response_format` object (docs/backends/openai-compatible.md). Accepts a
 * bare schema, a `{"name":…,"schema":…}` json_schema object, or a full
 * `{"type":"json_schema",…}` wrapper. Returns malloc'd compact JSON, or
 * NULL when the input is not a JSON object. This normalized shape is what
 * ctx->output_schema stores regardless of the wire in use. */
char *tny_openai_response_format(const char *schema_json, size_t len);

/* Responses API wire translation (responses.c, docs/adr/0016). Sessions
 * store Chat Completions-shaped messages; these translate at request time.
 * All return malloc'd compact JSON, or NULL on bad input. */

/* messages[boundary..] (+ an optional leading system summary) → the
 * Responses `input` items array: string messages ride as-is, image parts
 * become input_text/input_image, assistant tool_calls become function_call
 * items, and role:tool messages become function_call_output items. */
char *tny_openai_responses_input(yyjson_mut_val *msgs, int boundary,
                                 const char *summary);
/* Nested chat tools ({"type":"function","function":{…}}) → the flat
 * Responses shape ({"type":"function","name":…,"parameters":…}). */
char *tny_openai_responses_tools(const char *chat_tools_json);
/* Chat `response_format` wrapper → the flattened Responses `text.format`
 * object ({"type":"json_schema","name":…,"schema":…}). */
char *tny_openai_responses_text_format(const char *response_format_json);

#endif
