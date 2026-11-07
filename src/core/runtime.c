#include "core/runtime.h"
#include "core/acp_bridge.h"
#include "backends/acp/acp_client.h"
#include "core/skills.h"
#include "core/tasks.h"

#include "backends/openai/openai.h"
#include "lib/host_services.h"
#include "util/alloc.h"
#include "util/tny_poll.h"
#include "util/tny_wake.h"
#include "util/util.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_EVENT_MAX         256u
#define ENGINE_EVENT_BYTES_MAX   (1024u * 1024u)
#define ENGINE_RESERVED_EVENTS   2u
#define ENGINE_RESERVED_BYTES    1024u
#define ACP_TRANSCRIPT_BYTES_MAX (8u * 1024u * 1024u)

struct tny_engine {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_perm_decision (*prompt)(const char *, const char *, void *);
    void *prompt_ud;
    tny_engine_ask_user_cb ask_user;
    void *ask_user_ud;
    tny_engine_control_pump_cb control_pump;
    void *control_pump_ud;
    const char *session_sock;
    const char *session_id;
    tny_engine_cancel_probe cancel_probe;
    void *cancel_probe_ud;
    tny_wake cancel_wake;
    atomic_bool cancel_requested;
    atomic_bool cancel_armed;
    bool threadsafe_cancel;

    tny_backend *bk;
    bool active;
    bool terminal;
    bool terminal_popped;
    bool finalize_pending;
    bool overflow_pending;
    bool oom_pending;
    bool forcing_error;
    tny_stop_reason stop;
    char *prompt_text;

    tny_owned_event *pending_terminal;
    buf_t acp_text; /* durable external-agent answer */
    bool acp_iteration_pending;
    bool turn_started;

    tny_owned_event *head;
    tny_owned_event *tail;
    size_t queue_count;
    size_t queue_bytes;
    tny_owned_event *oom_error_reserve;
    tny_owned_event *oom_terminal_reserve;
};

static int64_t engine_monotonic_ms(const tny_engine *e) {
    return tny_host_services_monotonic_or_native(e->ctx->host_services);
}

static char *dup_bytes(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dup_cstr(const char *s) { return s ? dup_bytes(s, strlen(s)) : NULL; }

static tny_owned_event *event_copy(tny_engine *e, const tny_backend_event *ev) {
    uint64_t sequence = ++e->session->event_sequence;
    int64_t timestamp_ms = engine_monotonic_ms(e);
    char turn_id[160];
    snprintf(turn_id, sizeof turn_id, "%s:%llu:0", e->session->id,
             (unsigned long long)e->session->agent_sequence);
    tny_owned_event *o =
        tny_owned_event_copy(ev, tny_provider_name(e->ctx), e->session->id, turn_id, 0);
    if (o) {
        o->sequence = sequence;
        o->timestamp_ms = timestamp_ms;
    }
    return o;
}

static tny_owned_event *reserve_event(tny_engine *e, tny_event_kind kind) {
    tny_backend_event ev = {0};
    ev.kind = kind;
    if (kind == TNY_EV_ERROR) {
        ev.text = "out of memory";
        ev.text_len = strlen(ev.text);
        ev.error_code = TNY_EVENT_ERROR_OOM;
    } else ev.stop = TNY_STOP_ERROR;
    return tny_owned_event_copy(&ev, tny_provider_name(e->ctx), e->session->id, "", 192);
}

/* Replenish a consumed OOM pair only while no turn is active. Public events
 * remain independently owned by their caller, so recycling them from
 * tny_event_free would couple event lifetime to the session. Building both
 * replacements transactionally also leaves a retryable state if allocation
 * fails part-way through the pre-turn re-arm. */
static bool ensure_oom_reserves(tny_engine *e) {
    if (e->oom_error_reserve && e->oom_terminal_reserve) return true;
    tny_owned_event *error = e->oom_error_reserve;
    tny_owned_event *terminal = e->oom_terminal_reserve;
    bool new_error = false;
    bool new_terminal = false;
    if (!error) {
        error = reserve_event(e, TNY_EV_ERROR);
        new_error = true;
    }
    if (error && !terminal) {
        terminal = reserve_event(e, TNY_EV_TURN_END);
        new_terminal = true;
    }
    if (!error || !terminal) {
        if (new_error) tny_owned_event_free(error);
        if (new_terminal) tny_owned_event_free(terminal);
        return false;
    }
    e->oom_error_reserve = error;
    e->oom_terminal_reserve = terminal;
    return true;
}

static void prepare_reserved_event(tny_engine *e, tny_owned_event *o) {
    o->sequence = ++e->session->event_sequence;
    o->timestamp_ms = engine_monotonic_ms(e);
    tny_owned_event_set_turn(o, e->session->id, e->session->agent_sequence, 0);
    o->next = NULL;
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
    tny_host_services_notify_advisory(e->ctx->host_services);
}

static void queue_event(tny_engine *e, const tny_backend_event *ev) {
    if (e->terminal) return; /* duplicate/post-terminal events ignored */
    if (tny_alloc_scope_failed()) {
        e->oom_pending = true;
        return;
    }

    /* Hold the provider terminal outside the frontend queue until fallible
     * finalization (session persistence) has succeeded. */
    if (ev->kind == TNY_EV_TURN_END && e->pending_terminal) return;

    tny_owned_event *copy = event_copy(e, ev);
    if (!copy) {
        if (tny_alloc_scope_failed()) e->oom_pending = true;
        else e->overflow_pending = true;
        return;
    }
    bool reserved = is_reserved_kind(ev->kind);
    size_t count_limit = reserved ? ENGINE_EVENT_MAX : ENGINE_EVENT_MAX - ENGINE_RESERVED_EVENTS;
    size_t byte_limit =
        reserved ? ENGINE_EVENT_BYTES_MAX : ENGINE_EVENT_BYTES_MAX - ENGINE_RESERVED_BYTES;
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
    if (e->bk && e->bk->id == TNY_BK_ACP && ev->kind == TNY_EV_TEXT_DELTA && ev->text) {
        if (ev->text_len > ACP_TRANSCRIPT_BYTES_MAX - e->acp_text.len) {
            const char *message = "ACP transcript exceeds the 8 MiB limit";
            tny_backend_event failure = {.kind = TNY_EV_ERROR,
                                         .text = message,
                                         .text_len = strlen(message),
                                         .error_code = TNY_EVENT_ERROR_BACKPRESSURE};
            e->forcing_error = true;
            e->overflow_pending = true;
            queue_event(e, &failure);
            return;
        }
        buf_append(&e->acp_text, ev->text, ev->text_len);
        if (buf_oom(&e->acp_text)) {
            e->oom_pending = true;
            return;
        }
    }
    queue_event(e, ev);
}

static void synth_error(tny_engine *e, tny_event_error_kind code, const char *text) {
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

/* ACP owns its remote conversation, while tny still needs a durable local
 * answer for job results, dependency hashes, inspection and ordinary resume.
 * Only normalized assistant text is stored; no native model-call usage is inferred. */
static int persist_acp_iteration(tny_engine *e) {
    if (!e->acp_iteration_pending || !e->bk || e->bk->id != TNY_BK_ACP) return 0;
    if (tny_alloc_scope_failed()) return -1;
    session_add_assistant(e->session, e->acp_text.data ? e->acp_text.data : "", NULL);
    if (tny_alloc_scope_failed()) return -1;
    session_bump_turns(e->session);
    if (tny_alloc_scope_failed()) return -1;
    e->acp_iteration_pending = false;
    return session_save(e->session);
}

static int start_backend_iteration(tny_engine *e, const char *prompt, const char **images,
                                   char *err, size_t errlen) {
    e->active = true;
    e->turn_started = true;
    if (e->bk->id == TNY_BK_ACP) {
        buf_clear(&e->acp_text);
        session_add_text(e->session, "user", prompt);
        e->acp_iteration_pending = true;
        if (tny_alloc_scope_failed() || session_save(e->session) != 0) {
            e->forcing_error = true;
            if (!tny_alloc_scope_failed()) {
                synth_error(e, TNY_EVENT_ERROR_IO, "could not persist ACP prompt");
                synth_terminal(e, TNY_STOP_ERROR);
            }
            return 0;
        }
    }
    int rc = e->bk->send(e->bk, prompt, images, backend_event, e, err, errlen);
    if (rc != 0) {
        e->forcing_error = true;
        synth_error(e, TNY_EVENT_ERROR_IO, err && *err ? err : "backend send failed");
        synth_terminal(e, TNY_STOP_ERROR);
        return 0; /* a started lifecycle always drains to a terminal event */
    }
    return 0;
}

static void finalize_turn(tny_engine *e);

static void commit_pending_terminal(tny_engine *e) {
    tny_owned_event *terminal = e->pending_terminal;
    if (!terminal) return;
    /* Keep the terminal private until fallible finalization succeeds. */
    bool was_active = e->active;
    e->active = false;
    e->stop = terminal->ev.stop;
    e->finalize_pending = true;
    finalize_turn(e);
    if (tny_alloc_scope_failed()) {
        e->active = was_active;
        tny_engine_fail_oom(e);
        return;
    }
    e->pending_terminal = NULL;
    e->terminal = true;
    atomic_store_explicit(&e->cancel_armed, false, memory_order_release);
    e->stop = terminal->ev.stop;
    append_owned(e, terminal);
}

static void resolve_pending_terminal(tny_engine *e) {
    if (!e->pending_terminal) return;
    if (persist_acp_iteration(e) != 0 && !tny_alloc_scope_failed()) {
        e->forcing_error = true;
        e->pending_terminal->ev.stop = TNY_STOP_ERROR;
        synth_error(e, TNY_EVENT_ERROR_IO, "could not persist ACP answer");
    }
    commit_pending_terminal(e);
    e->turn_started = false;
}

static void finalize_turn(tny_engine *e) {
    if (!e->finalize_pending || !e->session || !e->bk || tny_alloc_scope_failed()) return;
    e->finalize_pending = false;
    if (e->bk->session_pointer) {
        char *ptr = e->bk->session_pointer(e->bk);
        if (ptr && !tny_alloc_scope_failed()) session_set_host_pointer(e->session, ptr);
        free(ptr);
    }
    if (tny_alloc_scope_failed()) return;
    session_set_meta(e->session, tny_provider_name(e->ctx), e->ctx->model);
    if (tny_alloc_scope_failed()) return;
    if (!session_title(e->session) && e->prompt_text) session_set_title(e->session, e->prompt_text);
    if (tny_alloc_scope_failed()) return;
    /* Native requests apply a reversible byte-budgeted working view. Do not
     * automatically truncate user constraints into the legacy turn summary. */
    session_save(e->session);
}

static void after_backend(tny_engine *e, int dispatch_rc) {
    if (tny_alloc_scope_failed()) e->oom_pending = true;
    if (e->oom_pending && !e->terminal) {
        tny_engine_fail_oom(e);
    } else if (e->overflow_pending && !e->terminal && !e->pending_terminal) {
        e->overflow_pending = false;
        e->forcing_error = true;
        synth_error(e, TNY_EVENT_ERROR_BACKPRESSURE, "event queue backpressure");
        if (e->bk && e->bk->cancel) e->bk->cancel(e->bk);
        synth_terminal(e, TNY_STOP_ERROR);
    } else if (dispatch_rc != 0 && !e->terminal && !e->pending_terminal) {
        synth_error(e, TNY_EVENT_ERROR_IO, "backend transport failed");
        synth_terminal(e, TNY_STOP_ERROR);
    }
    resolve_pending_terminal(e);
    finalize_turn(e);
}

tny_engine *tny_engine_new(tny_ctx *ctx, tny_session_state *session, perm_engine *perm,
                           tny_perm_decision (*prompt)(const char *, const char *, void *),
                           void *prompt_ud) {
    if (!ctx || !session || !perm) return NULL;
    tny_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ctx = ctx;
    e->session = session;
    e->perm = perm;
    e->prompt = prompt;
    e->prompt_ud = prompt_ud;
    e->cancel_wake.read_fd = -1;
    e->cancel_wake.write_fd = -1;
    atomic_init(&e->cancel_requested, false);
    atomic_init(&e->cancel_armed, false);
    buf_init(&e->acp_text);
    if (!ensure_oom_reserves(e)) {
        tny_engine_free(e);
        return NULL;
    }
    return e;
}

static void drop_backend(tny_engine *e) {
    if (!e || !e->bk) return;
    e->bk->disconnect(e->bk);
    e->bk->destroy(e->bk);
    e->bk = NULL;
}

static bool native_tool_cancelled(void *ud) {
    tny_engine *e = ud;
    return atomic_load_explicit(&e->cancel_requested, memory_order_acquire) ||
           (e->cancel_probe && e->cancel_probe(e->cancel_probe_ud));
}

int tny_engine_prepare(tny_engine *e, tny_backend *prepared, tny_engine_prepare_state state,
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
    if (state == TNY_ENGINE_PREPARE_FRESH && bk->connect(bk, err, errlen) != 0) {
        drop_backend(e);
        return -1;
    }
    if (bk->id == TNY_BK_OPENAI)
        tny_backend_openai_bind(bk, e->session, e->perm, e->prompt, e->prompt_ud, e->ask_user,
                                e->ask_user_ud, e->control_pump, e->control_pump_ud,
                                e->session_sock, e->session_id, NULL, NULL);
    if (bk->id == TNY_BK_OPENAI) tny_backend_openai_set_tool_cancel(bk, native_tool_cancelled, e);
    if (bk->id == TNY_BK_ACP) {
        tools_env env = {.ctx = e->ctx,
                         .session = e->session,
                         .perm = e->perm,
                         .prompt = e->prompt,
                         .prompt_ud = e->prompt_ud,
                         .ask_user = e->ask_user,
                         .ask_user_ud = e->ask_user_ud,
                         .control_pump = e->control_pump,
                         .control_pump_ud = e->control_pump_ud,
                         .session_sock = e->session_sock,
                         .session_id = e->session_id,
                         .cancelled = native_tool_cancelled,
                         .cancelled_ud = e};
        tny_acp_bridge_bind(tny_backend_acp_bridge(bk), &env, NULL, NULL);
    }
    if (state != TNY_ENGINE_PREPARE_RESUMED && bk->create_or_resume) {
        const char *ptr = session_host_pointer(e->session);
        const char *owner = session_backend(e->session);
        if (ptr && owner && strcmp(owner, tny_provider_name(e->ctx)) != 0) ptr = NULL;
        if (bk->create_or_resume(bk, ptr, err, errlen) != 0) {
            drop_backend(e);
            return -1;
        }
    }
    return 0;
}

void tny_engine_set_frontend_control(tny_engine *e, tny_engine_ask_user_cb ask_user,
                                     void *ask_user_ud, tny_engine_control_pump_cb control_pump,
                                     void *control_pump_ud, const char *session_sock,
                                     const char *session_id) {
    if (!e || e->bk || e->active) return;
    e->ask_user = ask_user;
    e->ask_user_ud = ask_user_ud;
    e->control_pump = control_pump;
    e->control_pump_ud = control_pump_ud;
    e->session_sock = session_sock;
    e->session_id = session_id;
}

int tny_engine_queue_image(tny_engine *e, const char *path, char *err, size_t errlen) {
    /* Configuration refusal precedes the transport check and every read or
     * queue mutation (docs/adr/0089). */
    if (e && tny_image_input_refused(e->ctx)) {
        if (err && errlen) snprintf(err, errlen, "%s", TNY_IMAGE_INPUT_REFUSAL);
        return -1;
    }
    if (!e || !e->bk || e->bk->id != TNY_BK_OPENAI) {
        if (err && errlen) snprintf(err, errlen, "image attach is unavailable on this backend");
        return -1;
    }
    return tny_backend_openai_queue_image(e->bk, path, err, errlen);
}

tny_image_preview_status tny_engine_queue_image_preview(tny_engine *e, const char *path,
                                                        const char *expected_sha256,
                                                        uint64_t expected_bytes,
                                                        const char **code_out, char *err,
                                                        size_t errlen) {
    if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_NO_SESSION;
    /* The configuration refusal precedes the transport check and every read,
     * exactly like the manual queue entry point (docs/adr/0089). */
    if (e && tny_image_input_refused(e->ctx)) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_CAPABILITY;
        if (err && errlen) snprintf(err, errlen, "%s", TNY_IMAGE_INPUT_REFUSAL);
        return TNY_IMAGE_PREVIEW_UNSUPPORTED;
    }
    /* No owning native turn to attach to: never a queued success from an idle
     * or completed session (A15 D1). */
    if (!e || !e->bk || e->bk->id != TNY_BK_OPENAI || !e->active) {
        if (err && errlen) snprintf(err, errlen, "no active native turn can take an image preview");
        return TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION;
    }
    return tny_backend_openai_queue_image_preview(e->bk, path, expected_sha256, expected_bytes,
                                                  code_out, err, errlen);
}

void tny_engine_set_cancel_probe(tny_engine *e, tny_engine_cancel_probe probe, void *ud) {
    if (!e) return;
    e->cancel_probe = probe;
    e->cancel_probe_ud = ud;
}

int tny_engine_enable_threadsafe_cancel(tny_engine *e) {
    if (!e || e->threadsafe_cancel || e->active) return -1;
    if (tny_wake_init(&e->cancel_wake) != 0) return -1;
    e->threadsafe_cancel = true;
    return 0;
}

void tny_engine_request_cancel(tny_engine *e) {
    if (!e || !e->threadsafe_cancel ||
        !atomic_load_explicit(&e->cancel_armed, memory_order_acquire))
        return;
    if (!atomic_exchange_explicit(&e->cancel_requested, true, memory_order_acq_rel))
        tny_wake_signal(&e->cancel_wake);
}

int tny_engine_start(tny_engine *e, const char *prompt, const char **images, char *err,
                     size_t errlen) {
    if (!e || !e->bk || !prompt || e->active || e->head || e->pending_terminal ||
        (e->terminal && !e->terminal_popped)) {
        if (err && errlen) snprintf(err, errlen, "runtime is not ready for a turn");
        return -1;
    }
    if (yyjson_mut_obj_get(yyjson_mut_doc_get_root(e->session->doc), "continuation")) {
        snprintf(err, errlen,
                 "saved continuation requires tny resume; a new prompt cannot repair or replay "
                 "pending calls");
        return -1;
    }
    /* The shared image gate (docs/adr/0089): every turn — TUI, one-shot CLI,
     * detached runner, library caller and native subagent — passes here, so
     * a configured-false provider refuses before prompt, event, session or
     * reserve state changes. */
    if (images && images[0] && tny_image_input_refused(e->ctx)) {
        if (err && errlen) snprintf(err, errlen, "%s", TNY_IMAGE_INPUT_REFUSAL);
        return -1;
    }
    if (!ensure_oom_reserves(e)) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
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
    atomic_store_explicit(&e->cancel_requested, false, memory_order_release);
    tny_wake_drain(&e->cancel_wake);
    e->session->agent_sequence++;
    buf_t effective;
    buf_init(&effective);
    buf_appends(&effective, prompt);
    if (buf_oom(&effective) || tny_alloc_scope_failed()) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        buf_free(&effective);
        return -1;
    }
    /* Skill mentions (docs/adr/0056): `/name` or `$name` in the typed text
     * puts that SKILL.md ahead of the user turn for every backend — the
     * system prompt stays cacheable and hosts never see tny's skill tool.
     * Recorded only once the send was entered, keyed by the message index the
     * native backend appends next, so transcripts still show `prompt`. */
    char **skill_names = NULL;
    int n_skill_names = 0;
    int skill_message_index = session_message_count(e->session);
    if (!e->ctx->prompt_optimisation) {
        char *with_skills = skills_inject(e->ctx, e->session, effective.data ? effective.data : "",
                                          (e->bk->id == TNY_BK_OPENAI || e->bk->id == TNY_BK_ACP),
                                          &skill_names, &n_skill_names);
        if (with_skills) {
            buf_clear(&effective);
            buf_appends(&effective, with_skills);
            free(with_skills);
            if (buf_oom(&effective)) {
                if (err && errlen) snprintf(err, errlen, "out of memory");
                buf_free(&effective);
                skills_names_free(skill_names, n_skill_names);
                return -1;
            }
        }
    }
    int rc = start_backend_iteration(e, effective.data, images, err, errlen);
    if (rc == 0 && n_skill_names && e->turn_started)
        session_record_skill_injection(e->session, skill_message_index, skill_names, n_skill_names,
                                       prompt);
    skills_names_free(skill_names, n_skill_names);
    buf_free(&effective);
    if (rc != 0) {
        tny_owned_event *queued;
        while ((queued = tny_engine_pop_event(e))) tny_owned_event_free(queued);
        e->terminal_popped = false;
        return -1; /* start failure creates no event stream */
    }
    if (e->active) atomic_store_explicit(&e->cancel_armed, true, memory_order_release);
    after_backend(e, 0);
    return 0;
}

int tny_engine_steer(tny_engine *e, const char *text, char *err, size_t errlen) {
    if (!e || !e->active || !e->bk || !e->bk->steer) {
        if (err && errlen) snprintf(err, errlen, "turn cannot accept steering");
        return -1;
    }
    int rc = e->bk->steer(e->bk, text, err, errlen);
    if (rc != 0) {
        tny_backend_event rejected = {0};
        rejected.kind = TNY_EV_STEER_REJECTED;
        rejected.text = text;
        rejected.text_len = strlen(text);
        backend_event(&rejected, e);
        after_backend(e, 0);
        rc = 0; /* runtime owns the queued rejection */
    }
    return rc;
}

void tny_engine_cancel(tny_engine *e) {
    if (!e || !e->active || !e->bk || !e->bk->cancel) return;
    e->bk->cancel(e->bk);
    after_backend(e, 0);
}

void tny_engine_fail_oom(tny_engine *e) {
    if (!e || e->terminal) return;
    tny_alloc_settlement_begin();
    tny_alloc_scope_clear();
    e->oom_pending = false;
    e->overflow_pending = false;
    e->forcing_error = true;
    e->finalize_pending = false; /* persistence is not emergency settlement */
    /* Publish settlement before cancel: backend cancellation callbacks must
     * not construct another event while the reserved pair is being used. */
    e->terminal = true;
    if (e->bk && e->active && e->bk->cancel) e->bk->cancel(e->bk);
    if (e->pending_terminal) {
        tny_owned_event_free(e->pending_terminal);
        e->pending_terminal = NULL;
    }
    tny_owned_event *error = e->oom_error_reserve;
    tny_owned_event *terminal = e->oom_terminal_reserve;
    e->oom_error_reserve = NULL;
    e->oom_terminal_reserve = NULL;
    if (error) {
        prepare_reserved_event(e, error);
        append_owned(e, error);
    }
    if (terminal) {
        prepare_reserved_event(e, terminal);
        append_owned(e, terminal);
    }
    e->terminal = true;
    e->active = false;
    e->terminal_popped = false;
    e->stop = TNY_STOP_ERROR;
    atomic_store_explicit(&e->cancel_armed, false, memory_order_release);
    tny_alloc_settlement_end();
}

void tny_engine_respond_permission(tny_engine *e, const char *id, tny_perm_decision decision) {
    if (!e || !e->active || !e->bk || !e->bk->respond_permission) return;
    e->bk->respond_permission(e->bk, id, decision);
    after_backend(e, 0);
}

int tny_engine_pollfds(tny_engine *e, struct pollfd *fds, int max) {
    if (!e || !e->active || !e->bk || !e->bk->pollfds) return 0;
    return e->bk->pollfds(e->bk, fds, max);
}

int tny_engine_poll_timeout(tny_engine *e, int timeout_ms) {
    if (!e || !e->active || !e->bk || !e->bk->poll_timeout) return timeout_ms;
    int backend_timeout = e->bk->poll_timeout(e->bk);
    return backend_timeout >= 0 && (timeout_ms < 0 || backend_timeout < timeout_ms)
               ? backend_timeout
               : timeout_ms;
}

int tny_engine_dispatch(tny_engine *e, struct pollfd *fds, int n) {
    if (!e || !e->active || !e->bk || !e->bk->dispatch) return 0;
    int rc = e->bk->dispatch(e->bk, fds, n);
    after_backend(e, rc);
    return rc;
}

tny_owned_event *tny_engine_pop_event(tny_engine *e) {
    if (!e) return NULL;
    if (!e->head) return NULL;
    tny_owned_event *o = e->head;
    e->head = o->next;
    if (!e->head) e->tail = NULL;
    o->next = NULL;
    e->queue_count--;
    e->queue_bytes -= o->owned_bytes;
    if (o->ev.kind == TNY_EV_TURN_END) e->terminal_popped = true;
    return o;
}

tny_engine_next tny_engine_next_event(tny_engine *e, int timeout_ms, tny_owned_event **out,
                                      char *err, size_t errlen) {
    if (out) *out = NULL;
    if (!e || !out || timeout_ms < 0) {
        if (err && errlen) snprintf(err, errlen, "invalid next_event arguments");
        return TNY_ENGINE_NEXT_ERROR;
    }
    if (e->threadsafe_cancel &&
        atomic_exchange_explicit(&e->cancel_requested, false, memory_order_acq_rel) && e->active)
        tny_engine_cancel(e);
    tny_owned_event *queued = tny_engine_pop_event(e);
    if (queued) {
        *out = queued;
        return TNY_ENGINE_NEXT_EVENT;
    }
    if (e->terminal_popped) return TNY_ENGINE_NEXT_DRAINED;
    if (!e->active) {
        if (err && errlen) snprintf(err, errlen, "no turn is active");
        return TNY_ENGINE_NEXT_ERROR;
    }

    int64_t deadline = engine_monotonic_ms(e) + timeout_ms;
    do {
        struct pollfd fds[TNY_BACKEND_POLLFD_MAX + 1];
        int n = tny_engine_pollfds(e, fds, TNY_BACKEND_POLLFD_MAX);
        int backend_n = n;
        int wake_index = -1;
        if (e->threadsafe_cancel) {
            wake_index = n++;
            fds[wake_index].fd = tny_wake_fd(&e->cancel_wake);
            fds[wake_index].events = POLLIN;
            fds[wake_index].revents = 0;
        }
        int remaining = (int)(deadline - engine_monotonic_ms(e));
        if (remaining < 0) remaining = 0;
        remaining = tny_engine_poll_timeout(e, remaining);
        int pr = tny_poll(n ? fds : NULL, (nfds_t)n, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            e->forcing_error = true;
            synth_error(e, TNY_EVENT_ERROR_IO, "runtime poll failed");
            if (e->bk && e->bk->cancel) e->bk->cancel(e->bk);
            synth_terminal(e, TNY_STOP_ERROR);
            after_backend(e, 0);
        } else {
            if (wake_index >= 0 && fds[wake_index].revents) {
                tny_wake_drain(&e->cancel_wake);
                if (atomic_exchange_explicit(&e->cancel_requested, false, memory_order_acq_rel) &&
                    e->active)
                    tny_engine_cancel(e);
            }
            tny_engine_dispatch(e, fds, backend_n);
        }
        queued = tny_engine_pop_event(e);
        if (queued) {
            *out = queued;
            return TNY_ENGINE_NEXT_EVENT;
        }
        if (e->terminal_popped) return TNY_ENGINE_NEXT_DRAINED;
        if (engine_monotonic_ms(e) >= deadline) return TNY_ENGINE_NEXT_TIMEOUT;
    } while (e->active);

    return e->terminal_popped ? TNY_ENGINE_NEXT_DRAINED : TNY_ENGINE_NEXT_TIMEOUT;
}

bool tny_engine_ready(const tny_engine *e) { return e && e->bk; }
tny_backend_id tny_engine_backend_id(const tny_engine *e) {
    return e && e->bk ? e->bk->id : TNY_BK_COUNT;
}

int tny_engine_openai_steps(tny_engine *e) {
    return tny_engine_backend_id(e) == TNY_BK_OPENAI ? tny_backend_openai_steps(e->bk) : 1;
}

const char *tny_engine_openai_toolcalls_json(tny_engine *e) {
    return tny_engine_backend_id(e) == TNY_BK_OPENAI ? tny_backend_openai_toolcalls_json(e->bk)
                                                     : "[]";
}

char *tny_engine_openai_usage_json(tny_engine *e) {
    if (!e || !e->bk) return NULL;
    if (e->bk->id == TNY_BK_ACP) return tny_backend_acp_usage_json(e->bk);
    return e->bk->id == TNY_BK_OPENAI ? tny_backend_openai_usage_json(e->bk) : NULL;
}

int tny_engine_compact(tny_engine *e, bool force) {
    if (!e || !e->session || e->active) return -1;
    if (!session_compact_needed(e->session, force)) return 0;
    if (tny_alloc_scope_failed()) return -1;
    int changed = session_compact(e->session, force);
    if (tny_alloc_scope_failed()) return -1;
    int saved = changed >= 0 ? session_save(e->session) : -1;
    if (tny_alloc_scope_failed()) return -1;
    return changed >= 0 && saved == 0 ? changed : -1;
}

void tny_engine_free(tny_engine *e) {
    if (!e) return;
    atomic_store_explicit(&e->cancel_armed, false, memory_order_release);
    if (e->active) tny_engine_cancel(e);
    drop_backend(e);
    tny_owned_event *event;
    while ((event = tny_engine_pop_event(e))) tny_owned_event_free(event);
    tny_owned_event_free(e->pending_terminal);
    tny_owned_event_free(e->oom_error_reserve);
    tny_owned_event_free(e->oom_terminal_reserve);
    buf_free(&e->acp_text);
    free(e->prompt_text);
    tny_wake_close(&e->cancel_wake);
    free(e);
}

bool tny_engine_parked(tny_engine *e) {
    return e && e->active && e->bk && e->bk->id == TNY_BK_OPENAI &&
           tny_backend_openai_parked(e->bk);
}

yyjson_mut_val *tny_engine_checkpoint(tny_engine *e, yyjson_mut_doc *d) {
    if (!tny_engine_parked(e) || e->head || e->pending_terminal) return NULL;
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_val *native = tny_backend_openai_checkpoint(e->bk, d);
    if (!native) return NULL;
    yyjson_mut_obj_add_val(d, r, "native", native);
    if (e->prompt_text) yyjson_mut_obj_add_strcpy(d, r, "prompt_text", e->prompt_text);
    yyjson_mut_obj_add_bool(d, r, "turn_started", e->turn_started);
    return r;
}

int tny_engine_restore(tny_engine *e, yyjson_val *r) {
    if (!e || !e->bk || e->bk->id != TNY_BK_OPENAI || e->active || !yyjson_is_obj(r)) return -1;
    if (!ensure_oom_reserves(e) ||
        tny_backend_openai_restore(e->bk, jget(r, "native"), backend_event, e) != 0)
        return -1;
    free(e->prompt_text);
    e->prompt_text = jget_str(r, "prompt_text") ? xstrdup(jget_str(r, "prompt_text")) : NULL;
    e->turn_started = jget_bool(r, "turn_started", false);
    e->active = true;
    e->terminal = e->terminal_popped = false;
    return 0;
}

int tny_engine_continue(tny_engine *e) {
    if (!tny_engine_parked(e)) return -1;
    int rc = tny_backend_openai_continue(e->bk);
    after_backend(e, rc);
    return rc;
}

void tny_engine_handoff_free(tny_engine *e) {
    if (!tny_engine_parked(e)) return;
    e->active = false; /* ownership transfer, never a cancelled turn */
    tny_engine_free(e);
}
