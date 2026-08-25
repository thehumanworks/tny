/* extension_caps.c — one frozen vocabulary, truthful per-provider state. */
#include "core/extension_caps.h"

#include "json/json.h"
#include "util/util.h"

static const char *CAPABILITY_NAMES[TNY_EXT_CAP_COUNT] = {
    [TNY_EXT_CAP_PROMPT_OBSERVE] = "extensions.prompt.observe",
    [TNY_EXT_CAP_PROMPT_TRANSFORM] = "extensions.prompt.transform",
    [TNY_EXT_CAP_PROMPT_BLOCK] = "extensions.prompt.block",
    [TNY_EXT_CAP_LIFECYCLE_SESSION_OBSERVE] =
        "extensions.lifecycle.session.observe",
    [TNY_EXT_CAP_LIFECYCLE_TURN_OBSERVE] =
        "extensions.lifecycle.turn.observe",
    [TNY_EXT_CAP_LIFECYCLE_MESSAGE_OBSERVE] =
        "extensions.lifecycle.message.observe",
    [TNY_EXT_CAP_LIFECYCLE_COMPACTION_OBSERVE] =
        "extensions.lifecycle.compaction.observe",
    [TNY_EXT_CAP_LIFECYCLE_MODEL_OBSERVE] =
        "extensions.lifecycle.model.observe",
    [TNY_EXT_CAP_LIFECYCLE_EFFORT_OBSERVE] =
        "extensions.lifecycle.effort.observe",
    [TNY_EXT_CAP_LIFECYCLE_INSTRUCTIONS_OBSERVE] =
        "extensions.lifecycle.instructions.observe",
    [TNY_EXT_CAP_LIFECYCLE_WORKSPACE_OBSERVE] =
        "extensions.lifecycle.workspace.observe",
    [TNY_EXT_CAP_LIFECYCLE_SUBAGENT_OBSERVE] =
        "extensions.lifecycle.subagent.observe",
    [TNY_EXT_CAP_TOOL_PRE_OBSERVE] = "extensions.tool.pre.observe",
    [TNY_EXT_CAP_TOOL_PRE_REWRITE] = "extensions.tool.pre.rewrite",
    [TNY_EXT_CAP_TOOL_PRE_DENY] = "extensions.tool.pre.deny",
    [TNY_EXT_CAP_PERMISSION_OBSERVE] = "extensions.permission.observe",
    [TNY_EXT_CAP_PERMISSION_ALLOW_ONCE] = "extensions.permission.allow_once",
    [TNY_EXT_CAP_PERMISSION_DENY] = "extensions.permission.deny",
    [TNY_EXT_CAP_PERMISSION_ABSTAIN] = "extensions.permission.abstain",
    [TNY_EXT_CAP_TOOL_POST_OBSERVE] = "extensions.tool.post.observe",
    [TNY_EXT_CAP_TOOL_POST_ANNOTATE] = "extensions.tool.post.annotate",
    [TNY_EXT_CAP_TOOL_POST_REPLACE] = "extensions.tool.post.replace",
    [TNY_EXT_CAP_TOOL_BATCH_OBSERVE] = "extensions.tool.batch.observe",
    [TNY_EXT_CAP_PROVIDER_REQUEST_OBSERVE_REDACTED] =
        "extensions.provider.request.observe_redacted",
    [TNY_EXT_CAP_PROVIDER_RESPONSE_OBSERVE_REDACTED] =
        "extensions.provider.response.observe_redacted",
    [TNY_EXT_CAP_AGENT_CONTINUE] = "extensions.agent.continue",
    [TNY_EXT_CAP_AGENT_CANCEL] = "extensions.agent.cancel",
    [TNY_EXT_CAP_PROJECT_LOCAL_DISCOVER] =
        "extensions.project_local.discover",
    [TNY_EXT_CAP_PROJECT_LOCAL_TRUST] = "extensions.project_local.trust",
};

#define S TNY_EXT_CAP_SUPPORTED
#define X TNY_EXT_CAP_UNSUPPORTED
#define U TNY_EXT_CAP_UNAVAILABLE

/* #54 reports shipping truth.  Contracted #55 controls remain unavailable
 * until their transaction and ordering are implemented.  Host-owned controls
 * with no protocol decision surface are unsupported, never approximated. */
static const tny_extension_capability_state OPENAI_CAPS[TNY_EXT_CAP_COUNT] = {
    S, U, U, S, U, U, U, U, U, U, U, U, U, U, U,
    S, U, U, U, S, U, U, U, U, U, S, S, U, U,
};

static const tny_extension_capability_state CODEX_CAPS[TNY_EXT_CAP_COUNT] = {
    S, U, U, S, U, U, U, U, U, U, U, U, U, X, X,
    S, U, U, U, S, X, X, U, U, U, S, S, U, U,
};

static const tny_extension_capability_state CURSOR_CAPS[TNY_EXT_CAP_COUNT] = {
    S, U, U, S, U, U, U, U, U, U, U, U, U, X, X,
    X, X, X, X, S, X, X, U, U, U, S, S, U, U,
};

static const tny_extension_capability_state ACP_CAPS[TNY_EXT_CAP_COUNT] = {
    S, U, U, S, U, U, U, U, U, U, U, U, U, X, X,
    S, U, U, U, S, X, X, U, U, U, S, S, U, U,
};

#undef S
#undef X
#undef U

static const tny_extension_capability_state *provider_caps(
    tny_backend_id provider) {
    switch (provider) {
    case TNY_BK_OPENAI: return OPENAI_CAPS;
    case TNY_BK_CURSOR: return CURSOR_CAPS;
    case TNY_BK_CODEX: return CODEX_CAPS;
    case TNY_BK_ACP: return ACP_CAPS;
    default: return NULL;
    }
}

size_t tny_extension_capability_count(void) {
    return TNY_EXT_CAP_COUNT;
}

const char *tny_extension_capability_name(tny_extension_capability_id id) {
    return id >= 0 && id < TNY_EXT_CAP_COUNT ? CAPABILITY_NAMES[id] : NULL;
}

tny_extension_capability_state tny_extension_capability_get(
    tny_backend_id provider, tny_extension_capability_id id) {
    const tny_extension_capability_state *caps = provider_caps(provider);
    if (!caps || id < 0 || id >= TNY_EXT_CAP_COUNT)
        return TNY_EXT_CAP_UNAVAILABLE;
    return caps[id];
}

const char *tny_extension_capability_state_name(
    tny_extension_capability_state state) {
    switch (state) {
    case TNY_EXT_CAP_SUPPORTED: return "supported";
    case TNY_EXT_CAP_UNSUPPORTED: return "unsupported";
    default: return "unavailable";
    }
}

const char *tny_extension_capability_reason(
    tny_backend_id provider, tny_extension_capability_id id) {
    if (!provider_caps(provider) || id < 0 || id >= TNY_EXT_CAP_COUNT)
        return "unknown_provider_or_capability";
    switch (tny_extension_capability_get(provider, id)) {
    case TNY_EXT_CAP_SUPPORTED:
        return "implemented";
    case TNY_EXT_CAP_UNAVAILABLE:
        return "contracted_not_implemented";
    case TNY_EXT_CAP_UNSUPPORTED:
        if (provider == TNY_BK_CURSOR &&
            id >= TNY_EXT_CAP_PERMISSION_OBSERVE &&
            id <= TNY_EXT_CAP_PERMISSION_ABSTAIN)
            return "protocol_missing";
        return "provider_owned";
    }
    return "unknown_state";
}

static void append_provider(buf_t *b, tny_backend_id provider) {
    const char *name = tny_backend_name(provider);
    buf_appends(b, "{");
    buf_appends(b, "\"provider\":");
    jescape(b, name);
    buf_appends(b, ",\"runtime\":");
    jescape(b, provider == TNY_BK_OPENAI ? "native" : "host");
    buf_appends(b, ",\"entries\":{");
    for (int i = 0; i < TNY_EXT_CAP_COUNT; i++) {
        if (i) buf_appends(b, ",");
        jescape(b, CAPABILITY_NAMES[i]);
        buf_appends(b, ":{\"state\":");
        jescape(b, tny_extension_capability_state_name(
            tny_extension_capability_get(provider,
                                         (tny_extension_capability_id)i)));
        buf_appends(b, ",\"reason\":");
        jescape(b, tny_extension_capability_reason(
            provider, (tny_extension_capability_id)i));
        buf_appends(b, "}");
    }
    buf_appends(b, "}}");
}

char *tny_extension_capabilities_json(tny_backend_id selected_provider,
                                      bool extensions_enabled,
                                      bool python_available) {
    const char *selected = tny_backend_name(selected_provider);
    if (!provider_caps(selected_provider)) selected = "unknown";
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"schema_version\":%d,\"selected_provider\":",
                TNY_EXTENSION_CAPABILITY_SCHEMA);
    jescape(&b, selected);
    buf_appendf(&b, ",\"extension_runtime\":{\"enabled\":%s,\"python\":",
                extensions_enabled ? "true" : "false");
    jescape(&b, python_available ? "available" : "unavailable");
    buf_appends(&b, "},\"providers\":{");
    for (int i = 0; i < TNY_BK_COUNT; i++) {
        if (i) buf_appends(&b, ",");
        jescape(&b, tny_backend_name((tny_backend_id)i));
        buf_appends(&b, ":");
        append_provider(&b, (tny_backend_id)i);
    }
    buf_appends(&b, "}}");
    return buf_detach(&b);
}
