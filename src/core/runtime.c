#include "core/runtime.h"

#include "backends/openai/openai.h"
#include "core/extension_caps.h"
#include "core/extensions.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_EVENT_MAX 256u
#define ENGINE_EVENT_BYTES_MAX (1024u * 1024u)
#define ENGINE_RESERVED_EVENTS 2u
#define ENGINE_RESERVED_BYTES 1024u

struct tny_engine {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_perm_decision (*prompt)(const char *, const char *, void *);
    void *prompt_ud;

    tny_backend *bk;
    bool active;
    bool terminal;
    bool terminal_popped;
    bool finalize_pending;
    bool overflow_pending;
    bool forcing_error;
    tny_stop_reason stop;
    char *prompt_text;
    char *prepared_requeue_text;

    tny_extensions *extensions;
    bool extensions_started;
    bool extension_stop_requested;
    bool extension_cancel_sent;
    uint64_t submission_sequence;
    int extension_continuations;
    tny_owned_event *pending_terminal;
    buf_t turn_text;
    buf_t message_text;
    char *current_message_id;
    bool message_started;
    bool turn_started;
    bool preserve_session_on_free;
    buf_t extension_followup;

    tny_owned_event *head;
    tny_owned_event *tail;
    size_t queue_count;
    size_t queue_bytes;
};

static char *dup_bytes(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dup_cstr(const char *s) {
    return s ? dup_bytes(s, strlen(s)) : NULL;
}

static void free_fields(tny_owned_event *o) {
    free((char *)o->ev.text);
    free((char *)o->ev.message_id);
    free((char *)o->ev.tool_name);
    free((char *)o->ev.tool_id);
    free((char *)o->ev.tool_detail);
    free((char *)o->ev.perm_id);
    free((char *)o->ev.perm_summary);
    free((char *)o->ev.message_type);
}

void tny_owned_event_free(tny_owned_event *o) {
    if (!o) return;
    free_fields(o);
    free(o);
}

static bool copy_field(const char *src, size_t n, const char **dst,
                       size_t *owned) {
    if (!src) { *dst = NULL; return true; }
    char *copy = dup_bytes(src, n);
    if (!copy) return false;
    *dst = copy;
    *owned += n + 1;
    return true;
}

static tny_owned_event *event_copy(const tny_backend_event *ev) {
    tny_owned_event *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->ev = *ev;
    if (!copy_field(ev->text, ev->text ? ev->text_len : 0,
                    &o->ev.text, &o->owned_bytes) ||
        !copy_field(ev->message_id,
                    ev->message_id ? strlen(ev->message_id) : 0,
                    &o->ev.message_id, &o->owned_bytes) ||
        !copy_field(ev->tool_name, ev->tool_name ? strlen(ev->tool_name) : 0,
                    &o->ev.tool_name, &o->owned_bytes) ||
        !copy_field(ev->tool_id, ev->tool_id ? strlen(ev->tool_id) : 0,
                    &o->ev.tool_id, &o->owned_bytes) ||
        !copy_field(ev->tool_detail, ev->tool_detail ? strlen(ev->tool_detail) : 0,
                    &o->ev.tool_detail, &o->owned_bytes) ||
        !copy_field(ev->perm_id, ev->perm_id ? strlen(ev->perm_id) : 0,
                    &o->ev.perm_id, &o->owned_bytes) ||
        !copy_field(ev->perm_summary,
                    ev->perm_summary ? strlen(ev->perm_summary) : 0,
                    &o->ev.perm_summary, &o->owned_bytes) ||
        !copy_field(ev->message_type,
                    ev->message_type ? strlen(ev->message_type) : 0,
                    &o->ev.message_type, &o->owned_bytes)) {
        tny_owned_event_free(o);
        return NULL;
    }
    return o;
}

static bool is_reserved_kind(tny_event_kind kind) {
    return kind == TNY_EV_ERROR || kind == TNY_EV_TURN_END;
}

static void append_owned(tny_engine *e, tny_owned_event *copy) {
    if (e->tail) e->tail->next = copy;
    else e->head = copy;
    e->tail = copy;
    e->queue_count++;
    e->queue_bytes += copy->owned_bytes;
}

static void queue_event(tny_engine *e, const tny_backend_event *ev) {
    if (e->terminal) return; /* duplicate/post-terminal events ignored */

    /* A provider terminal is only a candidate agent end. Hold it outside the
     * frontend queue until Python hooks have observed every preceding event
     * and agent_end has either continued or allowed settlement. */
    if (ev->kind == TNY_EV_TURN_END && e->pending_terminal) return;

    tny_owned_event *copy = event_copy(ev);
    if (!copy) { e->overflow_pending = true; return; }
    bool reserved = is_reserved_kind(ev->kind);
    size_t count_limit = reserved ? ENGINE_EVENT_MAX
                                  : ENGINE_EVENT_MAX - ENGINE_RESERVED_EVENTS;
    size_t byte_limit = reserved ? ENGINE_EVENT_BYTES_MAX
                                 : ENGINE_EVENT_BYTES_MAX - ENGINE_RESERVED_BYTES;
    if (e->queue_count >= count_limit || copy->owned_bytes > byte_limit ||
        e->queue_bytes > byte_limit - copy->owned_bytes) {
        tny_owned_event_free(copy);
        e->overflow_pending = true;
        return;
    }
    if (ev->kind == TNY_EV_TURN_END) {
        copy->ev.stop = e->forcing_error ? TNY_STOP_ERROR : ev->stop;
        e->pending_terminal = copy;
        return;
    }
    append_owned(e, copy);
}

static void backend_event(const tny_backend_event *ev, void *ud) {
    tny_engine *e = ud;
    if (e->terminal) return; /* suppress duplicate or post-terminal events */
    if (ev->kind == TNY_EV_STEER_REJECTED && ev->text) {
        free(e->prepared_requeue_text);
        e->prepared_requeue_text = dup_bytes(ev->text, ev->text_len);
    }
    queue_event(e, ev);
}

static void synth_error(tny_engine *e, tny_event_error_kind code,
                        const char *text) {
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_ERROR;
    ev.error_code = code;
    ev.text = text;
    ev.text_len = strlen(text);
    queue_event(e, &ev);
}

static void synth_terminal(tny_engine *e, tny_stop_reason stop) {
    if (e->terminal) return;
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    queue_event(e, &ev);
}

typedef enum {
    EXT_PHASE_PROMPT,
    EXT_PHASE_BEFORE_START,
    EXT_PHASE_STREAM,
    EXT_PHASE_AGENT_END,
    EXT_PHASE_OBSERVE
} extension_phase;

typedef struct {
    bool stop;
    bool continued;
    bool blocked;
    bool transformed;
    char extension[129];
    char reason[513];
} extension_fold;

static const char *event_kind_name(tny_event_kind kind) {
    switch (kind) {
    case TNY_EV_TEXT_DELTA: return "text_delta";
    case TNY_EV_THINKING: return "thinking";
    case TNY_EV_TOOL_START: return "tool_start";
    case TNY_EV_TOOL_END: return "tool_end";
    case TNY_EV_PERMISSION: return "permission_request";
    case TNY_EV_PLAN: return "plan";
    case TNY_EV_USAGE: return "usage";
    case TNY_EV_TURN_END: return "turn_end";
    case TNY_EV_ERROR: return "error";
    case TNY_EV_STATUS: return "status";
    case TNY_EV_STEER_REJECTED: return "steer_rejected";
    case TNY_EV_CUSTOM_MESSAGE: return "custom_message";
    case TNY_EV_USER_MESSAGE: return "user_message";
    case TNY_EV_TOOL_PROGRESS: return "tool_progress";
    }
    return "unknown";
}

static const char *stop_reason_name(tny_stop_reason stop) {
    switch (stop) {
    case TNY_STOP_DONE: return "done";
    case TNY_STOP_INTERRUPTED: return "interrupted";
    case TNY_STOP_DENIED: return "denied";
    case TNY_STOP_STEP_LIMIT: return "step_limit";
    case TNY_STOP_ERROR: return "error";
    }
    return "error";
}

static const char *event_error_name(tny_event_error_kind code) {
    switch (code) {
    case TNY_EVENT_ERROR_IO: return "io";
    case TNY_EVENT_ERROR_PROTOCOL: return "protocol";
    case TNY_EVENT_ERROR_BACKPRESSURE: return "backpressure";
    case TNY_EVENT_ERROR_AUTH: return "auth";
    case TNY_EVENT_ERROR_INTERNAL: return "internal";
    default: return "unknown";
    }
}

static void event_json_begin(tny_engine *e, buf_t *b, const char *type) {
    uint64_t seq = ++e->session->extension_event_sequence;
    char event_id[160];
    char turn_id[160];
    snprintf(event_id, sizeof event_id, "%s:%llu",
             e->session->id, (unsigned long long)seq);
    snprintf(turn_id, sizeof turn_id, "%s:%llu:%d",
             e->session->id,
             (unsigned long long)e->session->extension_agent_sequence,
             e->extension_continuations);
    buf_appends(b, "{\"schema_version\":1,\"event_id\":");
    jescape(b, event_id);
    buf_appends(b, ",\"type\":");
    jescape(b, type);
    buf_appendf(b, ",\"sequence\":%llu,\"provider\":",
                (unsigned long long)seq);
    jescape(b, tny_provider_name(e->ctx));
    buf_appends(b, ",\"session_id\":");
    jescape(b, e->session->id);
    buf_appends(b, ",\"turn_id\":");
    jescape(b, turn_id);
    buf_appendf(b, ",\"timestamp_ms\":%lld,\"payload\":{",
                (long long)monotonic_ms());
}

static char *backend_event_json(tny_engine *e, const tny_backend_event *ev) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, event_kind_name(ev->kind));
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
    case TNY_EV_THINKING:
    case TNY_EV_PLAN:
    case TNY_EV_STATUS:
    case TNY_EV_STEER_REJECTED:
    case TNY_EV_CUSTOM_MESSAGE:
    case TNY_EV_USER_MESSAGE:
        buf_appends(&b, "\"text\":");
        jescape(&b, ev->text ? ev->text : "");
        if (ev->message_id) {
            buf_appends(&b, ",\"message_id\":");
            jescape(&b, ev->message_id);
        }
        if (ev->message_type) {
            buf_appends(&b, ",\"custom_type\":");
            jescape(&b, ev->message_type);
        }
        break;
    case TNY_EV_TOOL_START:
    case TNY_EV_TOOL_END:
    case TNY_EV_TOOL_PROGRESS:
        buf_appends(&b, "\"tool_name\":");
        jescape(&b, ev->tool_name ? ev->tool_name : "tool");
        buf_appends(&b, ",\"tool_id\":");
        jescape(&b, ev->tool_id ? ev->tool_id : "");
        buf_appends(&b, ",\"detail\":");
        jescape(&b, ev->tool_detail ? ev->tool_detail : "");
        if (ev->kind == TNY_EV_TOOL_END)
            buf_appendf(&b, ",\"ok\":%s", ev->tool_ok ? "true" : "false");
        break;
    case TNY_EV_PERMISSION:
        buf_appends(&b, "\"permission_id\":");
        jescape(&b, ev->perm_id ? ev->perm_id : "");
        buf_appends(&b, ",\"summary\":");
        jescape(&b, ev->perm_summary ? ev->perm_summary : "");
        buf_appendf(&b, ",\"options\":%d", ev->perm_options);
        break;
    case TNY_EV_USAGE:
        buf_appendf(&b, "\"input_tokens\":%lld,\"output_tokens\":%lld,"
                        "\"context_used\":%lld,\"context_size\":%lld",
                    (long long)ev->in_tokens, (long long)ev->out_tokens,
                    (long long)ev->context_used, (long long)ev->context_size);
        if (ev->has_cost) buf_appendf(&b, ",\"cost\":%.12g", ev->cost);
        break;
    case TNY_EV_TURN_END:
        buf_appends(&b, "\"stop\":{\"reason\":");
        jescape(&b, stop_reason_name(ev->stop));
        buf_appends(&b, "}");
        break;
    case TNY_EV_ERROR:
        buf_appends(&b, "\"message\":");
        jescape(&b, ev->text ? ev->text : "");
        buf_appends(&b, ",\"error_code\":");
        jescape(&b, event_error_name(ev->error_code));
        break;
    }
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *lifecycle_event_json(tny_engine *e, const char *type,
                                  const char *prompt,
                                  const tny_backend_event *terminal) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, type);
    bool comma = false;
    if (prompt) {
        buf_appends(&b, "\"prompt\":");
        jescape(&b, prompt);
        comma = true;
    }
    if (terminal) {
        if (comma) buf_appends(&b, ",");
        buf_appends(&b, "\"stop\":{\"reason\":");
        jescape(&b, stop_reason_name(terminal->stop));
        buf_appends(&b, "},\"output_text\":");
        jescape(&b, e->turn_text.data ? e->turn_text.data : "");
        buf_appends(&b, ",\"messages\":[{\"role\":\"assistant\",\"content\":");
        jescape(&b, e->turn_text.data ? e->turn_text.data : "");
        buf_appends(&b, "}]");
        buf_appendf(&b, ",\"continuation_count\":%d,"
                        "\"max_continuations\":%d",
                    e->extension_continuations,
                    e->ctx->max_extension_iterations);
    }
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *session_event_json(tny_engine *e, const char *type,
                                const char *reason,
                                const char *previous_session_id) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, type);
    buf_appends(&b, "\"reason\":");
    jescape(&b, reason ? reason : "unknown");
    if (previous_session_id && *previous_session_id) {
        buf_appends(&b, ",\"previous_session_id\":");
        jescape(&b, previous_session_id);
    }
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *prompt_submit_json(tny_engine *e, const char *prompt,
                                const char **images, const char *source,
                                char *submission_id, size_t submission_id_len) {
    snprintf(submission_id, submission_id_len, "%s:%llu:prompt:%llu",
             e->session->id,
             (unsigned long long)e->session->extension_agent_sequence,
             (unsigned long long)++e->submission_sequence);
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, "user_prompt_submit");
    buf_appends(&b, "\"prompt\":");
    jescape(&b, prompt);
    buf_appends(&b, ",\"source\":");
    jescape(&b, source ? source : "user");
    buf_appends(&b, ",\"submission_id\":");
    jescape(&b, submission_id);
    buf_appends(&b, ",\"images\":[");
    for (int i = 0; images && images[i]; i++) {
        if (i) buf_appends(&b, ",");
        buf_appends(&b, "{\"path\":");
        jescape(&b, images[i]);
        buf_appends(&b, "}");
    }
    buf_appends(&b, "]}}");
    return buf_detach(&b);
}

static char *turn_start_json(tny_engine *e, const char *source) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, "turn_start");
    buf_appendf(&b, "\"iteration\":%d,\"source\":",
                e->extension_continuations);
    jescape(&b, source ? source : "user");
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *message_event_json(tny_engine *e, const char *type,
                                const char *message_id, const char *text) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, type);
    buf_appends(&b, "\"message_id\":");
    jescape(&b, message_id ? message_id : "");
    buf_appends(&b, ",\"role\":\"assistant\",\"content_type\":\"text\"");
    if (text) {
        buf_appends(&b, ",\"text\":");
        jescape(&b, text);
    }
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *compact_event_json(tny_engine *e, const char *type,
                                const char *trigger, int before_count,
                                int after_count, const char *summary,
                                const char *error) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, type);
    buf_appends(&b, "\"trigger\":");
    jescape(&b, trigger ? trigger : "manual");
    if (strcmp(type, "pre_compact") == 0) {
        buf_appendf(&b, ",\"message_count\":%d", before_count);
    } else if (strcmp(type, "post_compact") == 0) {
        buf_appendf(&b, ",\"before_count\":%d,\"after_count\":%d,"
                        "\"summary\":",
                    before_count, after_count);
        char *bounded = summary ? dup_bytes(summary,
            strlen(summary) < 16384 ? strlen(summary) : 16384) : xstrdup("");
        jescape(&b, bounded ? bounded : "");
        free(bounded);
    } else {
        buf_appends(&b, ",\"error\":");
        jescape(&b, error ? error : "compaction failed");
    }
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *selection_event_json(tny_engine *e, const char *type,
                                  const char *previous, const char *current,
                                  const char *source) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, type);
    buf_appends(&b, "\"previous\":");
    jescape(&b, previous ? previous : "default");
    buf_appends(&b, ",\"current\":");
    jescape(&b, current ? current : "default");
    buf_appends(&b, ",\"source\":");
    jescape(&b, source ? source : "runtime");
    buf_appends(&b, "}}");
    return buf_detach(&b);
}

static char *workspace_event_json(tny_engine *e, const char *action,
                                  const char *path) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, "workspace_change");
    buf_appends(&b, "\"action\":");
    jescape(&b, action ? action : "update");
    buf_appends(&b, ",\"path\":");
    jescape(&b, path ? path : "");
    buf_appends(&b, ",\"directories\":[");
    for (int i = 0; i < e->ctx->n_extra_dirs; i++) {
        if (i) buf_appends(&b, ",");
        jescape(&b, e->ctx->extra_dirs[i]);
    }
    buf_appends(&b, "]}}");
    return buf_detach(&b);
}

static char *instructions_event_json(tny_engine *e) {
    buf_t b;
    buf_init(&b);
    event_json_begin(e, &b, "instructions_change");
    buf_appends(&b, "\"paths\":[");
    for (int i = 0; i < e->ctx->n_instruction_paths; i++) {
        if (i) buf_appends(&b, ",");
        jescape(&b, e->ctx->instruction_paths[i]);
    }
    buf_appends(&b, "],\"digest\":");
    jescape(&b, e->ctx->instructions_digest);
    buf_appendf(&b, ",\"count\":%d}}", e->ctx->n_instruction_paths);
    return buf_detach(&b);
}

static void queue_internal(tny_engine *e, const tny_backend_event *ev) {
    tny_owned_event *before = e->tail;
    queue_event(e, ev);
    if (e->tail && e->tail != before) e->tail->hooks_done = true;
}

static void extension_status(tny_engine *e, const char *extension,
                             const char *event, const char *code,
                             const char *message) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "extension %.100s hook %.80s failed (%.40s): %.300s",
                extension ? extension : "extension", event ? event : "event",
                code ? code : "error", message ? message : "no detail");
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_STATUS;
    ev.text = b.data;
    ev.text_len = b.len;
    queue_internal(e, &ev);
    buf_free(&b);
}

static void append_custom_context(buf_t *prompt,
                                  const tny_extension_action *action) {
    if (prompt->len) buf_appends(prompt, "\n\n");
    buf_appendf(prompt, "[Context from tny extension %s",
                action->extension ? action->extension : "extension");
    if (action->custom_type)
        buf_appendf(prompt, " (%s)", action->custom_type);
    buf_appends(prompt, "]\n");
    buf_appends(prompt, action->content ? action->content : "");
    buf_appends(prompt, "\n[End extension context]");
}

static void append_user_followup(buf_t *prompt,
                                 const tny_extension_action *action) {
    if (prompt->len) buf_appends(prompt, "\n\n");
    buf_appends(prompt, action->content ? action->content : "");
}

static void queue_action_message(tny_engine *e,
                                 const tny_extension_action *action,
                                 bool custom) {
    if (!action->display || !action->content) return;
    tny_backend_event ev = {0};
    ev.kind = custom ? TNY_EV_CUSTOM_MESSAGE : TNY_EV_USER_MESSAGE;
    ev.text = action->content;
    ev.text_len = strlen(action->content);
    ev.message_type = custom
        ? (action->custom_type ? action->custom_type : action->extension) : NULL;
    queue_internal(e, &ev);
}

static extension_fold fold_extension_result(tny_engine *e, const char *event,
                                            extension_phase phase,
                                            tny_extension_result *result,
                                            buf_t *target) {
    extension_fold fold = {0};
    for (size_t i = 0; i < result->failure_count; i++) {
        tny_extension_failure *f = &result->failures[i];
        extension_status(e, f->extension, event, f->code, f->message);
    }
    for (size_t i = 0; i < result->action_count; i++) {
        tny_extension_action *a = &result->actions[i];
        if (a->kind != TNY_EXTENSION_ACTION_STOP) continue;
        if (phase != EXT_PHASE_OBSERVE) fold.stop = true;
        else extension_status(e, a->extension, event, "invalid_action",
                              "stop is not accepted on this event");
    }
    /* Stop has deterministic precedence: do not publish or queue context that
     * will not be delivered. */
    if (fold.stop) return fold;
    for (size_t i = 0; i < result->action_count; i++) {
        tny_extension_action *a = &result->actions[i];
        if (a->kind != TNY_EXTENSION_ACTION_PROMPT_BLOCK) continue;
        if (phase != EXT_PHASE_PROMPT) {
            extension_status(e, a->extension, event, "invalid_action",
                             "prompt_block is accepted only on user_prompt_submit");
            continue;
        }
        fold.blocked = true;
        snprintf(fold.extension, sizeof fold.extension, "%s",
                 a->extension ? a->extension : "extension");
        snprintf(fold.reason, sizeof fold.reason, "%s",
                 a->reason ? a->reason : "prompt blocked");
    }
    /* Explicit block beats every transformation/context action. */
    if (fold.blocked) return fold;
    for (size_t i = 0; i < result->action_count; i++) {
        tny_extension_action *a = &result->actions[i];
        if (a->kind == TNY_EXTENSION_ACTION_STOP) {
            continue;
        }
        if (a->kind == TNY_EXTENSION_ACTION_PROMPT_BLOCK) continue;
        if (a->kind == TNY_EXTENSION_ACTION_PROMPT_TRANSFORM) {
            if (phase != EXT_PHASE_PROMPT || !target) {
                extension_status(e, a->extension, event, "invalid_action",
                                 "prompt_transform is accepted only on user_prompt_submit");
                continue;
            }
            buf_clear(target);
            buf_appends(target, a->content ? a->content : "");
            fold.transformed = true;
            snprintf(fold.extension, sizeof fold.extension, "%s",
                     a->extension ? a->extension : "extension");
            continue;
        }
        if (a->kind == TNY_EXTENSION_ACTION_CONTEXT) {
            if (phase == EXT_PHASE_OBSERVE || phase == EXT_PHASE_PROMPT) {
                extension_status(e, a->extension, event, "invalid_action",
                                 "context is not accepted on this event");
                continue;
            }
            buf_t *destination = phase == EXT_PHASE_BEFORE_START
                ? target : &e->extension_followup;
            append_custom_context(destination, a);
            queue_action_message(e, a, true);
            if (phase == EXT_PHASE_AGENT_END) fold.continued = true;
            continue;
        }
        if (a->kind == TNY_EXTENSION_ACTION_CONTINUE) {
            if (phase != EXT_PHASE_AGENT_END) {
                extension_status(e, a->extension, event, "invalid_action",
                                 "continue is accepted only on agent_end");
                continue;
            }
            bool custom = a->message_kind == TNY_EXTENSION_MESSAGE_CUSTOM;
            if (custom) append_custom_context(&e->extension_followup, a);
            else append_user_followup(&e->extension_followup, a);
            queue_action_message(e, a, custom);
            fold.continued = true;
            continue;
        }
        if (a->kind >= TNY_EXTENSION_ACTION_TOOL_REWRITE)
            extension_status(e, a->extension, event, "invalid_action",
                             "tool control action is not accepted on this event");
    }
    return fold;
}

static extension_fold invoke_extensions(tny_engine *e, const char *event,
                                        const char *json, extension_phase phase,
                                        buf_t *target) {
    extension_fold fold = {0};
    if (!e->extensions || !json) return fold;
    tny_extension_result result;
    if (tny_extensions_invoke(e->extensions, event, json, &result) != 0) {
        extension_status(e, "extension-host", event, "manager_error",
                         "could not invoke extension handlers");
        return fold;
    }
    fold = fold_extension_result(e, event, phase, &result, target);
    tny_extension_result_free(&result);
    return fold;
}

static void extensions_session_start(tny_engine *e) {
    if (!e->extensions || e->session->extension_session_started) return;
    e->session->extension_session_started = true;
    e->extensions_started = true;
    char *json = session_event_json(
        e, "session_start", e->session->extension_start_reason,
        e->session->extension_previous_session_id);
    if (!json) return;
    (void)invoke_extensions(e, "session_start", json, EXT_PHASE_OBSERVE, NULL);
    free(json);
    if (e->ctx->backend == TNY_BK_OPENAI) {
        json = instructions_event_json(e);
        if (json) {
            (void)invoke_extensions(e, "instructions_change", json,
                                    EXT_PHASE_OBSERVE, NULL);
            free(json);
        }
    }
}

static extension_fold prepare_user_prompt(tny_engine *e, const char *prompt,
                                          const char **images,
                                          const char *source,
                                          buf_t *effective) {
    extension_fold fold = {0};
    buf_clear(effective);
    buf_appends(effective, prompt);
    if (!e->extensions) return fold;
    extensions_session_start(e);
    char submission_id[192];
    char *json = prompt_submit_json(e, prompt, images, source,
                                    submission_id, sizeof submission_id);
    if (json) {
        fold = invoke_extensions(e, "user_prompt_submit", json,
                                 EXT_PHASE_PROMPT, effective);
        free(json);
    }
    if (fold.transformed || fold.blocked) {
        session_record_prompt_audit(e->session, submission_id, prompt,
                                    effective->data ? effective->data : "",
                                    fold.blocked, fold.extension, fold.reason);
    }
    if (fold.blocked) {
        buf_t status;
        buf_init(&status);
        buf_appendf(&status, "extension %s blocked prompt: %s",
                    fold.extension[0] ? fold.extension : "extension",
                    fold.reason[0] ? fold.reason : "blocked");
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_STATUS;
        ev.text = status.data;
        ev.text_len = status.len;
        queue_internal(e, &ev);
        buf_free(&status);
    }
    return fold;
}

static int start_backend_iteration(tny_engine *e, const char *prompt,
                                   const char **images,
                                   const char *source,
                                   char *err, size_t errlen) {
    extensions_session_start(e);

    buf_t effective;
    buf_init(&effective);
    bool stop = false;
    char *json = lifecycle_event_json(e, "before_agent_start", prompt, NULL);
    if (json) {
        extension_fold fold = invoke_extensions(e, "before_agent_start", json,
                                                EXT_PHASE_BEFORE_START,
                                                &effective);
        stop = fold.stop;
        free(json);
    }
    if (effective.len) buf_appends(&effective, "\n\n");
    buf_appends(&effective, prompt);

    buf_clear(&e->turn_text);
    buf_clear(&e->message_text);
    free(e->current_message_id);
    e->current_message_id = NULL;
    e->message_started = false;
    e->turn_started = false;
    if (stop) {
        e->active = true;
        e->extension_stop_requested = true;
        synth_terminal(e, TNY_STOP_INTERRUPTED);
        buf_free(&effective);
        return 0;
    }
    json = lifecycle_event_json(e, "agent_start", effective.data, NULL);
    if (json) {
        (void)invoke_extensions(e, "agent_start", json, EXT_PHASE_OBSERVE,
                                NULL);
        free(json);
    }

    json = turn_start_json(e, source);
    if (json) {
        (void)invoke_extensions(e, "turn_start", json, EXT_PHASE_OBSERVE,
                                NULL);
        free(json);
    }

    e->active = true;
    e->turn_started = true;
    int rc = e->bk->send(e->bk, effective.data, images, backend_event, e,
                         err, errlen);
    buf_free(&effective);
    if (rc != 0) {
        e->forcing_error = true;
        synth_error(e, TNY_EVENT_ERROR_IO,
                    err && *err ? err : "backend send failed");
        synth_terminal(e, TNY_STOP_ERROR);
        return 0; /* a started lifecycle always drains to a terminal event */
    }
    return 0;
}

static void end_extension_message(tny_engine *e) {
    if (!e->extensions || !e->message_started) return;
    char *json = message_event_json(
        e, "message_end", e->current_message_id,
        e->message_text.data ? e->message_text.data : "");
    if (json) {
        (void)invoke_extensions(e, "message_end", json, EXT_PHASE_OBSERVE,
                                NULL);
        free(json);
    }
    e->message_started = false;
    free(e->current_message_id);
    e->current_message_id = NULL;
    buf_clear(&e->message_text);
}

static void start_extension_message(tny_engine *e,
                                    const tny_backend_event *ev) {
    const char *id = ev->message_id;
    char generated[192];
    if (!id || !*id) {
        snprintf(generated, sizeof generated, "%s:%llu:%d:message",
                 e->session->id,
                 (unsigned long long)e->session->extension_agent_sequence,
                 e->extension_continuations);
        id = generated;
    }
    if (e->message_started && e->current_message_id &&
        strcmp(e->current_message_id, id) == 0)
        return;
    end_extension_message(e);
    e->current_message_id = xstrdup(id);
    e->message_started = e->current_message_id != NULL;
    if (!e->message_started) return;
    char *json = message_event_json(e, "message_start", id, NULL);
    if (json) {
        (void)invoke_extensions(e, "message_start", json, EXT_PHASE_OBSERVE,
                                NULL);
        free(json);
    }
}

static void process_queued_extension_hooks(tny_engine *e) {
    if (!e->extensions) return;
    bool stop = false;
    for (tny_owned_event *owned = e->head; owned; owned = owned->next) {
        if (owned->hooks_done) continue;
        owned->hooks_done = true;
        if (owned->ev.kind == TNY_EV_TEXT_DELTA && owned->ev.text) {
            start_extension_message(e, &owned->ev);
            buf_append(&e->turn_text, owned->ev.text, owned->ev.text_len);
            buf_append(&e->message_text, owned->ev.text, owned->ev.text_len);
        }
        char *json = backend_event_json(e, &owned->ev);
        if (!json) continue;
        extension_fold fold = invoke_extensions(
            e, event_kind_name(owned->ev.kind), json, EXT_PHASE_STREAM, NULL);
        stop = stop || fold.stop;
        free(json);
        if (owned->ev.kind == TNY_EV_TEXT_DELTA && owned->ev.text &&
            e->message_started) {
            json = message_event_json(e, "message_update",
                                      e->current_message_id, owned->ev.text);
            if (json) {
                extension_fold update_fold = invoke_extensions(
                    e, "message_update", json, EXT_PHASE_STREAM, NULL);
                stop = stop || update_fold.stop;
                free(json);
            }
        }
    }
    if (stop) e->extension_stop_requested = true;
}

static void commit_pending_terminal(tny_engine *e) {
    tny_owned_event *terminal = e->pending_terminal;
    if (!terminal) return;
    e->pending_terminal = NULL;
    e->terminal = true;
    e->active = false;
    e->stop = terminal->ev.stop;
    terminal->hooks_done = true;
    append_owned(e, terminal);
    e->finalize_pending = true;
}

/* Returns true when the resolver queued or started work that needs another
 * pass through after_backend before settlement. */
static bool resolve_pending_terminal(tny_engine *e) {
    if (!e->pending_terminal) return false;
    if (!e->extensions) {
        commit_pending_terminal(e);
        return false;
    }
    tny_backend_event *terminal = &e->pending_terminal->ev;
    bool stop = e->extension_stop_requested;

    end_extension_message(e);

    char *json = NULL;
    if (e->turn_started) {
        json = backend_event_json(e, terminal);
        if (json) {
            extension_fold turn_fold = invoke_extensions(
                e, "turn_end", json, EXT_PHASE_STREAM, NULL);
            stop = stop || turn_fold.stop;
            free(json);
        }

        json = lifecycle_event_json(e, "agent_end", NULL, terminal);
        if (json) {
            extension_fold end_fold = invoke_extensions(
                e, "agent_end", json, EXT_PHASE_AGENT_END,
                &e->extension_followup);
            stop = stop || end_fold.stop;
            free(json);
        }
    }

    bool continue_requested = e->turn_started && e->extension_followup.len > 0;
    if (terminal->stop == TNY_STOP_INTERRUPTED) continue_requested = false;
    if (stop) continue_requested = false;
    int limit = e->ctx->max_extension_iterations;
    if (continue_requested && limit > 0 &&
        e->extension_continuations >= limit) {
        tny_backend_event status = {0};
        const char *message = "extension continuation limit reached; agent settled";
        status.kind = TNY_EV_STATUS;
        status.text = message;
        status.text_len = strlen(message);
        queue_internal(e, &status);
        continue_requested = false;
    }

    if (continue_requested) {
        char *prompt = xstrdup(e->extension_followup.data);
        buf_clear(&e->extension_followup);
        tny_owned_event_free(e->pending_terminal);
        e->pending_terminal = NULL;
        e->extension_continuations++;
        e->extension_stop_requested = false;
        e->extension_cancel_sent = false;
        char err[512];
        int rc = start_backend_iteration(e, prompt, NULL, "continuation",
                                         err, sizeof err);
        free(prompt);
        if (rc == 0) return true;
        e->forcing_error = true;
        synth_error(e, TNY_EVENT_ERROR_IO, err[0] ? err
                                                   : "extension continuation failed");
        synth_terminal(e, TNY_STOP_ERROR);
        return true;
    }

    buf_clear(&e->extension_followup);
    json = lifecycle_event_json(e, "agent_settled", NULL, terminal);
    if (json) {
        (void)invoke_extensions(e, "agent_settled", json, EXT_PHASE_OBSERVE,
                                NULL);
        free(json);
    }
    commit_pending_terminal(e);
    e->turn_started = false;
    return false;
}

static void finalize_turn(tny_engine *e) {
    if (!e->finalize_pending || !e->session || !e->bk) return;
    e->finalize_pending = false;
    if (e->bk->session_pointer) {
        char *ptr = e->bk->session_pointer(e->bk);
        if (ptr) {
            session_set_host_pointer(e->session, ptr);
            free(ptr);
        }
    }
    session_set_meta(e->session, tny_provider_name(e->ctx), e->ctx->model);
    if (!session_title(e->session) && e->prompt_text)
        session_set_title(e->session, e->prompt_text);
    if (e->stop == TNY_STOP_DONE)
        (void)tny_engine_compact(e, false, "threshold");
    session_save(e->session);
}

static void after_backend(tny_engine *e, int dispatch_rc) {
    if (e->overflow_pending && !e->terminal && !e->pending_terminal) {
        e->overflow_pending = false;
        e->forcing_error = true;
        synth_error(e, TNY_EVENT_ERROR_BACKPRESSURE,
                    "event queue backpressure");
        if (e->bk && e->bk->cancel) e->bk->cancel(e->bk);
        synth_terminal(e, TNY_STOP_ERROR);
    } else if (dispatch_rc != 0 && !e->terminal && !e->pending_terminal) {
        synth_error(e, TNY_EVENT_ERROR_IO, "backend transport failed");
        synth_terminal(e, TNY_STOP_ERROR);
    }
    for (;;) {
        process_queued_extension_hooks(e);
        if (e->extension_stop_requested && !e->extension_cancel_sent &&
            !e->pending_terminal && e->bk && e->bk->cancel) {
            e->extension_cancel_sent = true;
            e->bk->cancel(e->bk);
            /* A host may confirm asynchronously; return to its poll loop. */
        }
        if (!e->pending_terminal) break;
        if (resolve_pending_terminal(e)) continue;
        break;
    }
    finalize_turn(e);
}

tny_engine *tny_engine_new(tny_ctx *ctx, tny_session_state *session,
                           perm_engine *perm,
                           tny_perm_decision (*prompt)(const char *,
                                                       const char *, void *),
                           void *prompt_ud) {
    if (!ctx || !session || !perm) return NULL;
    tny_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ctx = ctx;
    e->session = session;
    e->perm = perm;
    e->prompt = prompt;
    e->prompt_ud = prompt_ud;
    buf_init(&e->turn_text);
    buf_init(&e->message_text);
    buf_init(&e->extension_followup);
    if (ctx->extensions_enabled && !ctx->library_mode)
        e->extensions = ctx->extensions; /* borrowed; ctx outlives engine */
    return e;
}

static void drop_backend(tny_engine *e) {
    if (!e || !e->bk) return;
    e->bk->disconnect(e->bk);
    e->bk->destroy(e->bk);
    e->bk = NULL;
}

int tny_engine_prepare(tny_engine *e, tny_backend *prepared,
                       tny_engine_prepare_state state,
                       char *err, size_t errlen) {
    if (!e || e->bk || e->active) {
        if (err && errlen) snprintf(err, errlen, "runtime is not ready for a backend");
        if (prepared) prepared->destroy(prepared);
        return -1;
    }
    tny_backend *bk = prepared;
    if (!bk) {
        if (err && errlen) snprintf(err, errlen, "a prepared backend is required");
        return -1;
    }
    e->bk = bk; /* ownership transfers before any fallible operation */
    if (state == TNY_ENGINE_PREPARE_FRESH &&
        bk->connect(bk, err, errlen) != 0) {
        drop_backend(e);
        return -1;
    }
    if (bk->id == TNY_BK_OPENAI)
        tny_backend_openai_bind(bk, e->session, e->perm, e->prompt, e->prompt_ud);
    if (state != TNY_ENGINE_PREPARE_RESUMED && bk->create_or_resume) {
        const char *ptr = session_host_pointer(e->session);
        const char *owner = session_backend(e->session);
        if (ptr && owner && strcmp(owner, tny_provider_name(e->ctx)) != 0)
            ptr = NULL;
        if (bk->create_or_resume(bk, ptr, err, errlen) != 0) {
            drop_backend(e);
            return -1;
        }
    }
    return 0;
}

int tny_engine_start(tny_engine *e, const char *prompt, const char **images,
                     char *err, size_t errlen) {
    if (!e || !e->bk || !prompt || e->active || e->head ||
        e->pending_terminal || (e->terminal && !e->terminal_popped)) {
        if (err && errlen) snprintf(err, errlen, "runtime is not ready for a turn");
        return -1;
    }
    free(e->prompt_text);
    e->prompt_text = dup_cstr(prompt);
    if (!e->prompt_text) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return -1;
    }
    e->terminal = false;
    e->terminal_popped = false;
    e->forcing_error = false;
    e->stop = TNY_STOP_ERROR;
    e->extension_stop_requested = false;
    e->extension_cancel_sent = false;
    e->extension_continuations = 0;
    e->session->extension_agent_sequence++;
    buf_clear(&e->extension_followup);
    buf_t effective;
    buf_init(&effective);
    extension_fold prompt_fold = {0};
    if (e->prepared_requeue_text &&
        strcmp(e->prepared_requeue_text, prompt) == 0) {
        buf_appends(&effective, prompt); /* mutating hook already consumed */
        free(e->prepared_requeue_text);
        e->prepared_requeue_text = NULL;
    } else {
        prompt_fold = prepare_user_prompt(e, prompt, images, "user", &effective);
    }
    if (prompt_fold.stop || prompt_fold.blocked) {
        e->active = true;
        e->turn_started = false;
        e->extension_stop_requested = prompt_fold.stop;
        session_save(e->session);
        if (prompt_fold.blocked) {
            free(e->prompt_text);
            e->prompt_text = NULL; /* blocked text never becomes session title */
        }
        synth_terminal(e, prompt_fold.blocked ? TNY_STOP_DENIED
                                              : TNY_STOP_INTERRUPTED);
        buf_free(&effective);
        after_backend(e, 0);
        return 0;
    }
    int rc = start_backend_iteration(e, effective.data, images, "user",
                                     err, errlen);
    buf_free(&effective);
    if (rc != 0) {
        tny_owned_event *queued;
        while ((queued = tny_engine_pop_event(e))) tny_owned_event_free(queued);
        e->terminal_popped = false;
        buf_clear(&e->extension_followup);
        return -1; /* start failure creates no event stream */
    }
    after_backend(e, 0);
    return 0;
}

int tny_engine_steer(tny_engine *e, const char *text,
                     char *err, size_t errlen) {
    if (!e || !e->active || !e->bk || !e->bk->steer) {
        if (err && errlen) snprintf(err, errlen, "turn cannot accept steering");
        return -1;
    }
    buf_t effective;
    buf_init(&effective);
    extension_fold fold = prepare_user_prompt(e, text, NULL, "steer", &effective);
    if (fold.blocked) {
        session_save(e->session);
        buf_free(&effective);
        after_backend(e, 0);
        return 0;
    }
    if (fold.stop) {
        e->extension_stop_requested = true;
        buf_free(&effective);
        after_backend(e, 0);
        return 0;
    }
    int rc = e->bk->steer(e->bk, effective.data, err, errlen);
    if (rc != 0) {
        tny_backend_event rejected = {0};
        rejected.kind = TNY_EV_STEER_REJECTED;
        rejected.text = effective.data;
        rejected.text_len = effective.len;
        backend_event(&rejected, e);
        after_backend(e, 0);
        rc = 0; /* runtime owns the transformed text and queued rejection */
    }
    buf_free(&effective);
    return rc;
}

void tny_engine_cancel(tny_engine *e) {
    if (!e || !e->active || !e->bk || !e->bk->cancel) return;
    e->bk->cancel(e->bk);
    after_backend(e, 0);
}

void tny_engine_respond_permission(tny_engine *e, const char *id,
                                   tny_perm_decision decision) {
    if (!e || !e->active || !e->bk || !e->bk->respond_permission) return;
    e->bk->respond_permission(e->bk, id, decision);
    after_backend(e, 0);
}

int tny_engine_pollfds(tny_engine *e, struct pollfd *fds, int max) {
    if (!e || !e->active || !e->bk || !e->bk->pollfds) return 0;
    return e->bk->pollfds(e->bk, fds, max);
}

int tny_engine_dispatch(tny_engine *e, struct pollfd *fds, int n) {
    if (!e || !e->active || !e->bk || !e->bk->dispatch) return 0;
    int rc = e->bk->dispatch(e->bk, fds, n);
    after_backend(e, rc);
    return rc;
}

tny_owned_event *tny_engine_pop_event(tny_engine *e) {
    if (!e || !e->head) return NULL;
    tny_owned_event *o = e->head;
    e->head = o->next;
    if (!e->head) e->tail = NULL;
    o->next = NULL;
    e->queue_count--;
    e->queue_bytes -= o->owned_bytes;
    if (o->ev.kind == TNY_EV_TURN_END) e->terminal_popped = true;
    return o;
}

tny_engine_next tny_engine_next_event(tny_engine *e, int timeout_ms,
                                      tny_owned_event **out,
                                      char *err, size_t errlen) {
    if (out) *out = NULL;
    if (!e || !out || timeout_ms < 0) {
        if (err && errlen) snprintf(err, errlen, "invalid next_event arguments");
        return TNY_ENGINE_NEXT_ERROR;
    }
    tny_owned_event *queued = tny_engine_pop_event(e);
    if (queued) { *out = queued; return TNY_ENGINE_NEXT_EVENT; }
    if (e->terminal_popped) return TNY_ENGINE_NEXT_DRAINED;
    if (!e->active) {
        if (err && errlen) snprintf(err, errlen, "no turn is active");
        return TNY_ENGINE_NEXT_ERROR;
    }

    int64_t deadline = monotonic_ms() + timeout_ms;
    do {
        struct pollfd fds[8];
        int n = tny_engine_pollfds(e, fds, 8);
        int remaining = (int)(deadline - monotonic_ms());
        if (remaining < 0) remaining = 0;
        int pr = tny_poll(n ? fds : NULL, (nfds_t)n, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            e->forcing_error = true;
            synth_error(e, TNY_EVENT_ERROR_IO, "runtime poll failed");
            if (e->bk && e->bk->cancel) e->bk->cancel(e->bk);
            synth_terminal(e, TNY_STOP_ERROR);
            after_backend(e, 0);
        } else {
            tny_engine_dispatch(e, fds, n);
        }
        queued = tny_engine_pop_event(e);
        if (queued) { *out = queued; return TNY_ENGINE_NEXT_EVENT; }
        if (e->terminal_popped) return TNY_ENGINE_NEXT_DRAINED;
        if (monotonic_ms() >= deadline)
            return TNY_ENGINE_NEXT_TIMEOUT;
    } while (e->active);

    return e->terminal_popped ? TNY_ENGINE_NEXT_DRAINED
                              : TNY_ENGINE_NEXT_TIMEOUT;
}

bool tny_engine_ready(const tny_engine *e) { return e && e->bk; }
tny_backend_id tny_engine_backend_id(const tny_engine *e) {
    return e && e->bk ? e->bk->id : TNY_BK_COUNT;
}

int tny_engine_openai_steps(tny_engine *e) {
    return tny_engine_backend_id(e) == TNY_BK_OPENAI
        ? tny_backend_openai_steps(e->bk) : 1;
}

const char *tny_engine_openai_toolcalls_json(tny_engine *e) {
    return tny_engine_backend_id(e) == TNY_BK_OPENAI
        ? tny_backend_openai_toolcalls_json(e->bk) : "[]";
}

void tny_engine_preserve_session_on_free(tny_engine *e) {
    if (e) e->preserve_session_on_free = true;
}

void tny_engine_end_session(tny_engine *e, const char *reason) {
    if (!e || !e->extensions || !e->session->extension_session_started) return;
    char *json = session_event_json(e, "session_end",
                                    reason && *reason ? reason : "exit", NULL);
    if (json) {
        (void)invoke_extensions(e, "session_end", json, EXT_PHASE_OBSERVE,
                                NULL);
        free(json);
    }
    e->session->extension_session_started = false;
    e->extensions_started = false;
}

int tny_engine_compact(tny_engine *e, bool force, const char *trigger) {
    if (!e || !e->session || e->active) return -1;
    if (!session_compact_needed(e->session, force)) return 0;
    int before = session_message_count(e->session);
    bool observable = e->extensions && e->session->extension_session_started &&
        tny_extension_capability_get((tny_backend_id)e->ctx->backend,
            TNY_EXT_CAP_LIFECYCLE_COMPACTION_OBSERVE) == TNY_EXT_CAP_SUPPORTED;
    if (observable) {
        char *json = compact_event_json(e, "pre_compact", trigger,
                                        before, before, NULL, NULL);
        if (json) {
            (void)invoke_extensions(e, "pre_compact", json, EXT_PHASE_OBSERVE,
                                    NULL);
            free(json);
        }
    }
    int changed = session_compact(e->session, force);
    if (changed >= 0 && session_save(e->session) == 0) {
        if (changed && observable) {
            const char *summary = NULL;
            int boundary = session_compact_boundary(e->session, &summary);
            int after = session_message_count(e->session) - boundary + 1;
            char *json = compact_event_json(e, "post_compact", trigger,
                                            before, after, summary, NULL);
            if (json) {
                (void)invoke_extensions(e, "post_compact", json,
                                        EXT_PHASE_OBSERVE, NULL);
                free(json);
            }
        }
        return changed;
    }
    if (observable) {
        char *json = compact_event_json(e, "compact_failed", trigger,
                                        before, before, NULL,
                                        "session persistence failed");
        if (json) {
            (void)invoke_extensions(e, "compact_failed", json,
                                    EXT_PHASE_OBSERVE, NULL);
            free(json);
        }
    }
    return -1;
}

static void notify_selection_change(tny_engine *e, const char *type,
                                    const char *previous, const char *current,
                                    const char *source) {
    if (!e || !e->extensions || !e->session->extension_session_started) return;
    const char *before = previous ? previous : "default";
    const char *after = current ? current : "default";
    if (strcmp(before, after) == 0) return;
    char *json = selection_event_json(e, type, previous, current, source);
    if (json) {
        (void)invoke_extensions(e, type, json, EXT_PHASE_OBSERVE, NULL);
        free(json);
    }
}

void tny_engine_model_changed(tny_engine *e, const char *previous,
                              const char *current, const char *source) {
    notify_selection_change(e, "model_change", previous, current, source);
}

void tny_engine_effort_changed(tny_engine *e, const char *previous,
                               const char *current, const char *source) {
    notify_selection_change(e, "effort_change", previous, current, source);
}

void tny_engine_workspace_changed(tny_engine *e, const char *action,
                                  const char *path) {
    if (!e || !e->extensions || !e->session->extension_session_started) return;
    char *json = workspace_event_json(e, action, path);
    if (json) {
        (void)invoke_extensions(e, "workspace_change", json,
                                EXT_PHASE_OBSERVE, NULL);
        free(json);
    }
}

void tny_engine_free(tny_engine *e) {
    if (!e) return;
    if (e->active) tny_engine_cancel(e);
    if (!e->preserve_session_on_free) tny_engine_end_session(e, "exit");
    drop_backend(e);
    tny_owned_event *event;
    while ((event = tny_engine_pop_event(e))) tny_owned_event_free(event);
    tny_owned_event_free(e->pending_terminal);
    buf_free(&e->turn_text);
    buf_free(&e->message_text);
    buf_free(&e->extension_followup);
    free(e->prompt_text);
    free(e->prepared_requeue_text);
    free(e->current_message_id);
    free(e);
}
