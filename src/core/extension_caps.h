/* extension_caps.h — immutable extension capability vocabulary and matrices.
 *
 * This is an internal CLI/runtime contract.  It is deliberately separate from
 * the small TNY_CAP_* command-line feature bitmask and from libtny ABI 0. */
#ifndef TNY_EXTENSION_CAPS_H
#define TNY_EXTENSION_CAPS_H

#include "core/backend.h"

#include <stdbool.h>
#include <stddef.h>

#define TNY_EXTENSION_CAPABILITY_SCHEMA 1

typedef enum {
    TNY_EXT_CAP_SUPPORTED = 0,
    TNY_EXT_CAP_UNSUPPORTED,
    TNY_EXT_CAP_UNAVAILABLE
} tny_extension_capability_state;

typedef enum {
    TNY_EXT_CAP_PROMPT_OBSERVE = 0,
    TNY_EXT_CAP_PROMPT_TRANSFORM,
    TNY_EXT_CAP_PROMPT_BLOCK,
    TNY_EXT_CAP_LIFECYCLE_SESSION_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_TURN_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_MESSAGE_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_COMPACTION_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_MODEL_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_EFFORT_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_INSTRUCTIONS_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_WORKSPACE_OBSERVE,
    TNY_EXT_CAP_LIFECYCLE_SUBAGENT_OBSERVE,
    TNY_EXT_CAP_TOOL_PRE_OBSERVE,
    TNY_EXT_CAP_TOOL_PRE_REWRITE,
    TNY_EXT_CAP_TOOL_PRE_DENY,
    TNY_EXT_CAP_PERMISSION_OBSERVE,
    TNY_EXT_CAP_PERMISSION_ALLOW_ONCE,
    TNY_EXT_CAP_PERMISSION_DENY,
    TNY_EXT_CAP_PERMISSION_ABSTAIN,
    TNY_EXT_CAP_TOOL_POST_OBSERVE,
    TNY_EXT_CAP_TOOL_POST_ANNOTATE,
    TNY_EXT_CAP_TOOL_POST_REPLACE,
    TNY_EXT_CAP_TOOL_BATCH_OBSERVE,
    TNY_EXT_CAP_PROVIDER_REQUEST_OBSERVE_REDACTED,
    TNY_EXT_CAP_PROVIDER_RESPONSE_OBSERVE_REDACTED,
    TNY_EXT_CAP_AGENT_CONTINUE,
    TNY_EXT_CAP_AGENT_CANCEL,
    TNY_EXT_CAP_PROJECT_LOCAL_DISCOVER,
    TNY_EXT_CAP_PROJECT_LOCAL_TRUST,
    TNY_EXT_CAP_COUNT
} tny_extension_capability_id;

size_t tny_extension_capability_count(void);
const char *tny_extension_capability_name(tny_extension_capability_id id);
tny_extension_capability_state tny_extension_capability_get(tny_backend_id provider,
                                                            tny_extension_capability_id id);
const char *tny_extension_capability_state_name(tny_extension_capability_state state);
const char *tny_extension_capability_reason(tny_backend_id provider,
                                            tny_extension_capability_id id);

/* Deterministic, malloc'd JSON.  This query is static: it never discovers or
 * imports extensions, starts Python/a provider, reads credentials, or performs
 * network I/O. */
char *tny_extension_capabilities_json(tny_backend_id selected_provider, bool extensions_enabled,
                                      bool python_available);

#endif
