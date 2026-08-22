/* openai.h — native backend extras: the frontend binds the session,
 * permission engine, and approval hook before send(). */
#ifndef TNY_OPENAI_H
#define TNY_OPENAI_H

#include "core/backend.h"
#include "core/session.h"
#include "core/perm.h"

void tny_backend_openai_bind(tny_backend *b, tny_session *session,
                             perm_engine *perm,
                             tny_perm_decision (*prompt)(const char *tool,
                                                         const char *summary,
                                                         void *ud),
                             void *prompt_ud);

/* Number of agent steps taken in the last turn + tool call log (JSON array
 * text, borrowed until next send). */
int tny_backend_openai_steps(tny_backend *b);
const char *tny_backend_openai_toolcalls_json(tny_backend *b);

/* Normalize a user-supplied JSON Schema into a Chat Completions
 * `response_format` object (docs/backends/openai-compatible.md). Accepts a
 * bare schema, a `{"name":…,"schema":…}` json_schema object, or a full
 * `{"type":"json_schema",…}` wrapper. Returns malloc'd compact JSON, or
 * NULL when the input is not a JSON object. This normalized shape is what
 * ctx->output_schema stores regardless of the wire in use. */
char *tny_openai_response_format(const char *schema_json, size_t len);

/* Responses API wire translation (responses.c, docs/adr/0014). Sessions
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
