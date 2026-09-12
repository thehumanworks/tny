/* event_jsonl.c — one canonical public event per line, and the checked
 * stdout seam that delivers them (docs/adr/0090).
 *
 * Field names come from sdk/schema/events.json, numeric kinds/stop reasons
 * from include/tny/tny.h, and the error code from the same public mapping
 * libtny's readers expose (src/lib/tny.c). Every key of an event's schema
 * row is present on every line, so a consumer never has to distinguish
 * "absent" from "empty"; unavailable strings are "" and an unreported cost
 * is null. tests/integration/test_ask_events_conformance.py compares this
 * output with the libtny readers on the same provider response, so a
 * divergence here fails a test rather than shipping a second dialect. */
#include "core/event_jsonl.h"
#include "util/tny_poll.h"

#include <tny/tny.h> /* the frozen numeric vocabulary, not a copy of it */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* The line carries the engine's enum values as numbers, so the private and
 * public vocabularies must stay identical. A renumbering breaks the build
 * here instead of silently renaming events on the wire. */
_Static_assert(TNY_EVENT_TEXT_DELTA == (unsigned)TNY_EV_TEXT_DELTA, "text_delta");
_Static_assert(TNY_EVENT_THINKING == (unsigned)TNY_EV_THINKING, "thinking");
_Static_assert(TNY_EVENT_TOOL_START == (unsigned)TNY_EV_TOOL_START, "tool_start");
_Static_assert(TNY_EVENT_TOOL_END == (unsigned)TNY_EV_TOOL_END, "tool_end");
_Static_assert(TNY_EVENT_PERMISSION == (unsigned)TNY_EV_PERMISSION, "permission_request");
_Static_assert(TNY_EVENT_PLAN == (unsigned)TNY_EV_PLAN, "plan");
_Static_assert(TNY_EVENT_USAGE == (unsigned)TNY_EV_USAGE, "usage");
_Static_assert(TNY_EVENT_TURN_END == (unsigned)TNY_EV_TURN_END, "turn_end");
_Static_assert(TNY_EVENT_ERROR == (unsigned)TNY_EV_ERROR, "error");
_Static_assert(TNY_EVENT_STATUS == (unsigned)TNY_EV_STATUS, "status");
_Static_assert(TNY_EVENT_STEER_REJECTED == (unsigned)TNY_EV_STEER_REJECTED, "steer_rejected");
_Static_assert(TNY_EVENT_CUSTOM_MESSAGE == (unsigned)TNY_EV_CUSTOM_MESSAGE, "custom_message");
_Static_assert(TNY_EVENT_USER_MESSAGE == (unsigned)TNY_EV_USER_MESSAGE, "user_message");
_Static_assert(TNY_EVENT_TOOL_PROGRESS == (unsigned)TNY_EV_TOOL_PROGRESS, "tool_progress");
_Static_assert(TNY_STOP_REASON_DONE == (unsigned)TNY_STOP_DONE, "stop done");
_Static_assert(TNY_STOP_REASON_INTERRUPTED == (unsigned)TNY_STOP_INTERRUPTED, "stop interrupted");
_Static_assert(TNY_STOP_REASON_DENIED == (unsigned)TNY_STOP_DENIED, "stop denied");
_Static_assert(TNY_STOP_REASON_STEP_LIMIT == (unsigned)TNY_STOP_STEP_LIMIT, "stop step_limit");
_Static_assert(TNY_STOP_REASON_ERROR == (unsigned)TNY_STOP_ERROR, "stop error");
_Static_assert(TNY_PERM_ALLOW_ONCE == (int)TNY_PERMISSION_OPTION_ALLOW, "allow once");
_Static_assert(TNY_PERM_ALLOW_ALWAYS == (int)TNY_PERMISSION_OPTION_ALLOW_ALWAYS, "allow always");
_Static_assert(TNY_PERM_DENY == (int)TNY_PERMISSION_OPTION_DENY, "deny");

/* A POLLOUT-ready pipe accepts at least _POSIX_PIPE_BUF bytes without
 * blocking, so one chunk can never wedge the loop between cancel checks. */
#define JSONL_CHUNK   512
#define JSONL_WAIT_MS 100 /* stall slice: also the cancellation latency */

static void escape_bytes(buf_t *b, const char *s, size_t n) {
    buf_appends(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_appends(b, "\\\""); break;
        case '\\': buf_appends(b, "\\\\"); break;
        case '\n': buf_appends(b, "\\n"); break;
        case '\r': buf_appends(b, "\\r"); break;
        case '\t': buf_appends(b, "\\t"); break;
        default:
            if (c < 0x20) buf_appendf(b, "\\u%04x", c);
            else buf_append(b, &c, 1);
        }
    }
    buf_appends(b, "\"");
}

/* The public readers hand out an empty view for an absent string. */
static void escape_cstr(buf_t *b, const char *s) { escape_bytes(b, s ? s : "", s ? strlen(s) : 0); }

static void field_str(buf_t *b, const char *name, const char *s) {
    buf_appendf(b, ",\"%s\":", name);
    escape_cstr(b, s);
}

static const char *event_type(tny_event_kind kind) {
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

/* Same values tny_event_error_code() reports: the stable TNY_STATUS_*
 * categories, and OK for every event that is not an error. */
static int public_error_code(const tny_backend_event *ev) {
    if (ev->kind != TNY_EV_ERROR) return TNY_STATUS_OK;
    switch (ev->error_code) {
    case TNY_EVENT_ERROR_IO: return TNY_STATUS_IO;
    case TNY_EVENT_ERROR_PROTOCOL: return TNY_STATUS_PROTOCOL;
    case TNY_EVENT_ERROR_BACKPRESSURE: return TNY_STATUS_BACKPRESSURE;
    case TNY_EVENT_ERROR_AUTH: return TNY_STATUS_AUTH;
    case TNY_EVENT_ERROR_OOM: return TNY_STATUS_OOM;
    default: return TNY_STATUS_INTERNAL;
    }
}

static void text_fields(buf_t *b, const tny_backend_event *ev) {
    buf_appends(b, ",\"text\":");
    escape_bytes(b, ev->text ? ev->text : "", ev->text ? ev->text_len : 0);
    field_str(b, "message_id", ev->message_id);
}

static void tool_fields(buf_t *b, const tny_backend_event *ev) {
    field_str(b, "tool_name", ev->tool_name);
    field_str(b, "tool_id", ev->tool_id);
    field_str(b, "tool_detail", ev->tool_detail);
}

void tny_event_jsonl_append(buf_t *out, const tny_owned_event *event) {
    if (!out || !event) {
        if (out) out->oom = true;
        return;
    }
    const tny_backend_event *ev = &event->ev;
    /* envelope, in sdk/schema/events.json order, then the numeric kind the
     * public event view reports */
    buf_appendf(out, "{\"schema_version\":%u,\"sequence\":%llu,\"timestamp_ms\":%lld,\"provider\":",
                (unsigned)TNY_EVENT_SCHEMA_VERSION, (unsigned long long)event->sequence,
                (long long)event->timestamp_ms);
    escape_cstr(out, event->provider);
    field_str(out, "session_id", event->session_id);
    field_str(out, "turn_id", event->turn_id);
    field_str(out, "type", event_type(ev->kind));
    buf_appendf(out, ",\"kind\":%u", (unsigned)ev->kind);
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
    case TNY_EV_THINKING:
    case TNY_EV_PLAN:
    case TNY_EV_STATUS:
    case TNY_EV_STEER_REJECTED:
    case TNY_EV_USER_MESSAGE: text_fields(out, ev); break;
    case TNY_EV_CUSTOM_MESSAGE:
        text_fields(out, ev);
        field_str(out, "message_type", ev->message_type);
        break;
    case TNY_EV_TOOL_START:
    case TNY_EV_TOOL_PROGRESS: tool_fields(out, ev); break;
    case TNY_EV_TOOL_END:
        tool_fields(out, ev);
        buf_appendf(out, ",\"tool_ok\":%s", ev->tool_ok ? "true" : "false");
        break;
    case TNY_EV_PERMISSION:
        field_str(out, "permission_id", ev->perm_id);
        field_str(out, "permission_summary", ev->perm_summary);
        buf_appendf(out, ",\"permission_options\":%d", ev->perm_options);
        break;
    case TNY_EV_USAGE:
        buf_appendf(out,
                    ",\"input_tokens\":%lld,\"output_tokens\":%lld,\"context_used\":%lld,"
                    "\"context_size\":%lld,\"cost\":",
                    (long long)ev->in_tokens, (long long)ev->out_tokens,
                    (long long)ev->context_used, (long long)ev->context_size);
        if (ev->has_cost) buf_appendf(out, "%.12g", ev->cost);
        else buf_appends(out, "null");
        buf_appendf(out, ",\"has_cost\":%s", ev->has_cost ? "true" : "false");
        break;
    case TNY_EV_TURN_END: buf_appendf(out, ",\"stop_reason\":%u", (unsigned)ev->stop); break;
    case TNY_EV_ERROR:
        buf_appends(out, ",\"text\":");
        escape_bytes(out, ev->text ? ev->text : "", ev->text ? ev->text_len : 0);
        buf_appendf(out, ",\"error_code\":%d", public_error_code(ev));
        break;
    }
    buf_appends(out, "}\n");
}

void tny_event_writer_init(tny_event_writer *w, int fd, bool (*interrupted)(void *ud), void *ud) {
    if (!w) return;
    memset(w, 0, sizeof *w);
    buf_init(&w->line);
    w->fd = fd;
    w->interrupted = interrupted;
    w->ud = ud;
}

void tny_event_writer_free(tny_event_writer *w) {
    if (w) buf_free(&w->line);
}

static tny_event_write_rc write_all(tny_event_writer *w, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        if (w->interrupted && w->interrupted(w->ud)) return TNY_EVENT_WRITE_CANCELLED;
        struct pollfd pf = {w->fd, POLLOUT, 0};
        int pr = tny_poll(&pf, 1, JSONL_WAIT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            w->last_errno = errno;
            return TNY_EVENT_WRITE_IO;
        }
        /* Not writable within the slice is backpressure and nothing else: a
         * consumer that never reads delays delivery while every slice
         * re-checks the signal flag. The seam answers for this descriptor on
         * every build — the browser's reports its own stdout readiness
         * (src/net/net_wasm.c) — so there is no case where the writer has to
         * guess, and no blocking write() that an interrupt cannot reach. */
        if (pr == 0) continue;
        size_t chunk = len - off < JSONL_CHUNK ? len - off : JSONL_CHUNK;
        ssize_t n = write(w->fd, data + off, chunk);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        w->last_errno = n < 0 ? errno : EIO;
        return TNY_EVENT_WRITE_IO;
    }
    return TNY_EVENT_WRITE_OK;
}

tny_event_write_rc tny_event_writer_emit(tny_event_writer *w, const tny_owned_event *ev) {
    if (!w || !ev) return TNY_EVENT_WRITE_IO;
    buf_clear(&w->line);
    tny_event_jsonl_append(&w->line, ev);
    if (buf_oom(&w->line) || !w->line.data) {
        w->last_errno = ENOMEM;
        return TNY_EVENT_WRITE_IO;
    }
    return write_all(w, w->line.data, w->line.len);
}
