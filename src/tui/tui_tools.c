/* Tool display is reversible; preview text is never the backing store. */
#include "tui/tui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void call_free(tui_tool_call *c) {
    free(c->id);
    free(c->name);
    free(c->reason);
    free(c->args);
    free(c->result);
    free(c);
}

void tui_tools_free(tui *t) {
    while (t->tool_calls) {
        tui_tool_call *next = t->tool_calls->next;
        call_free(t->tool_calls);
        t->tool_calls = next;
    }
    t->tool_calls_tail = t->tool_view = NULL;
    buf_free(&t->tool_view_text);
    buf_free(&t->tool_view_rows);
    buf_free(&t->tool_view_visual);
}

/* Never pass tool-controlled terminal escapes through, even SGR. */
void tui_tool_literal(buf_t *out, const char *s, bool line) {
    if (!s) return;
    for (size_t i = 0, len = strlen(s); i < len;) {
        unsigned char ch = (unsigned char)s[i];
        size_t n = 1;
        while (ch >= 0xc0 && i + n < len && ((unsigned char)s[i + n] & 0xc0) == 0x80) n++;
        bool c1 = ch == 0xc2 && n == 2 && (unsigned char)s[i + 1] < 0xa0;
        if (ch >= 0x80) {
            if (!c1 && utf8_valid_bytes(s + i, n)) buf_append(out, s + i, n);
        } else if (ch == '\n' || ch == '\r' || ch == '\t')
            buf_appends(out, line ? " " : ch == '\t' ? "    " : "\n");
        else if (ch >= 0x20 && ch != 0x7f) buf_append(out, s + i, 1);
        i += n;
    }
}

static bool hidden(const tui *t) {
    return strcmp(tny_settings_ui_tool_call_view(t->ctx), "hidden") == 0;
}

static void heading(buf_t *out, const tui_tool_call *c, bool expanded) {
    buf_appendf(out, "%s #%zu %s ", expanded ? "▾" : "▸", c->number,
                c->done ? (c->ok ? "✓" : "✗") : "…");
    if (c->done && c->timed) buf_appendf(out, "%lld ms · ", (long long)c->elapsed_ms);
    tui_tool_literal(out, c->name, true);
    if (c->reason && *c->reason) {
        buf_appends(out, " · ");
        tui_tool_literal(out, c->reason, true);
    }
    buf_appends(out, "\n");
}

static void card(buf_t *out, const tui *t, const tui_tool_call *c, bool result) {
    if (hidden(t)) {
        if (result && c->done && !c->ok) {
            buf_appendf(out, "Tool #%zu failed: ", c->number);
            buf_t detail = {0};
            tui_tool_literal(&detail, c->result, false);
            tui_push_literal_row(out, detail.data, detail.len, 120);
            buf_free(&detail);
            buf_appendf(out, "\n  /tools %zu for complete details\n", c->number);
        }
        return;
    }
    if (!c->expanded) {
        if (result == !t->fullscreen || (result && c->start_at == SIZE_MAX)) {
            buf_t line = {0};
            heading(&line, c, false);
            tui_push_literal_row(out, line.data, line.len, t->cols > 3 ? t->cols - 1 : 3);
            buf_appends(out, "\n");
            buf_free(&line);
        }
        return;
    }
    heading(out, c, true);
    buf_appends(out, result ? "  Result:\n" : "  Arguments:\n");
    tui_tool_literal(out, result ? c->result : c->args, false);
    if (out->len && out->data[out->len - 1] != '\n') buf_appends(out, "\n");
}

static void replace_card(tui *t, tui_tool_call *c, bool result);

void tui_tools_trim(tui *t, size_t cut) {
    tui_tool_call **link = &t->tool_calls;
    t->tool_calls_tail = NULL;
    while (*link) {
        tui_tool_call *c = *link;
        if (c->start_at != SIZE_MAX) c->start_at = c->start_at < cut ? SIZE_MAX : c->start_at - cut;
        if (c->end_at != SIZE_MAX) c->end_at = c->end_at < cut ? SIZE_MAX : c->end_at - cut;
        if (c->done && c->start_at == SIZE_MAX && c->end_at == SIZE_MAX && t->tool_view != c &&
            !c->pinned && !c->restored) {
            *link = c->next;
            call_free(c);
        } else {
            t->tool_calls_tail = c;
            link = &c->next;
        }
    }
}

/* Hidden calls do not advance scrollback. Bound their retained details without
 * orphaning visible headings, while preserving active/viewed/newest calls. */
static bool invisible(const tui_tool_call *c) {
    return (!c->start_len || c->start_at == SIZE_MAX) && (!c->end_len || c->end_at == SIZE_MAX);
}

static size_t retained_bytes(const tui_tool_call *c) {
    return sizeof *c + strlen(c->id ? c->id : "") + strlen(c->name ? c->name : "") +
           strlen(c->reason ? c->reason : "") + strlen(c->args ? c->args : "") +
           strlen(c->result ? c->result : "");
}

static void retain_tail(tui *t) {
    size_t count = 0, bytes = 0;
    size_t limit = t->scrollback_lines ? t->scrollback_lines : TNY_UI_SCROLLBACK_LINES_DEFAULT;
    if (limit > 1024) limit = 1024;
    for (tui_tool_call *c = t->tool_calls; c; c = c->next) {
        if (!invisible(c)) continue;
        count++;
        bytes += retained_bytes(c);
    }
    tui_tool_call **link = &t->tool_calls;
    while (*link && (count > limit || bytes > 16u * 1024u * 1024u)) {
        tui_tool_call *c = *link;
        if (invisible(c) && c->done && !c->pinned && c != t->tool_view && c != t->tool_calls_tail) {
            count--;
            bytes -= retained_bytes(c);
            *link = c->next;
            call_free(c);
        } else link = &c->next;
    }
}

void tui_tools_event(tui *t, const tny_backend_event *ev) {
    bool result = ev->kind == TNY_EV_TOOL_END;
    tui_tool_call *c = NULL;
    if (result) {
        for (tui_tool_call *it = t->tool_calls; it; it = it->next)
            if (!it->done && ev->tool_id && it->id && strcmp(it->id, ev->tool_id) == 0) c = it;
    }
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) {
            tui_err(t, "could not retain tool details");
            return;
        }
        c->number = ++t->tool_sequence;
        c->restored = t->tool_restoring;
        c->start_at = c->end_at = SIZE_MAX;
        c->id = xstrdup(ev->tool_id ? ev->tool_id : "");
        c->name = xstrdup(ev->tool_name ? ev->tool_name : "tool");
        c->reason = xstrdup(ev->tool_reason ? ev->tool_reason : "");
        c->expanded = tny_settings_ui_tool_calls_expanded(t->ctx);
        c->timed = !t->tool_restoring && !result && t->event_timestamp_ms > 0;
        c->started_ms = c->timed ? t->event_timestamp_ms : 0;
        if (t->tool_calls_tail) t->tool_calls_tail->next = c;
        else t->tool_calls = c;
        t->tool_calls_tail = c;
    }
    c->pinned = true;
    if (result) {
        c->result = xstrdup(ev->tool_detail ? ev->tool_detail : "");
        c->done = true;
        c->ok = ev->tool_ok;
        if (c->timed) {
            if (t->event_timestamp_ms >= c->started_ms)
                c->elapsed_ms = t->event_timestamp_ms - c->started_ms;
            else c->timed = false;
        }
    } else c->args = xstrdup(ev->tool_detail ? ev->tool_detail : "");
    if (t->tool_restoring) {
        c->pinned = false;
        retain_tail(t);
        return;
    }
    tui_bol(t);
    tui_transcript_commit(t);
    if (result && t->fullscreen) replace_card(t, c, false);
    buf_t text = {0};
    card(&text, t, c, result);
    size_t at = t->transcript.len;
    if (text.len) tui_write(t, text.data, text.len);
    if (result) {
        c->end_at = at;
        c->end_order = ++t->tool_span_sequence;
        c->end_len = text.len;
    } else {
        c->start_at = at;
        c->start_order = ++t->tool_span_sequence;
        c->start_len = text.len;
    }
    buf_free(&text);
    tui_transcript_commit(t);
    if (t->tool_view == c) t->tool_view_changed = true;
    c->pinned = false;
    retain_tail(t);
}

static size_t newline_count(const char *s, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') count++;
    return count;
}

static void replace_card(tui *t, tui_tool_call *c, bool result) {
    size_t at = result ? c->end_at : c->start_at;
    size_t len = result ? c->end_len : c->start_len;
    if (at == SIZE_MAX || at > t->transcript.len || len > t->transcript.len - at) return;
    buf_t text = {0};
    card(&text, t, c, result);
    if (text.len == len && (!len || memcmp(t->transcript.data + at, text.data, len) == 0)) {
        buf_free(&text);
        return;
    }
    if (text.oom) {
        buf_free(&text);
        return;
    }
    if (text.len > len) buf_reserve(&t->transcript, text.len - len);
    if (t->transcript.oom) {
        buf_free(&text);
        return;
    }
    t->transcript_lines -= newline_count(t->transcript.data + at, len);
    t->transcript_lines += newline_count(text.data, text.len);
    memmove(t->transcript.data + at + text.len, t->transcript.data + at + len,
            t->transcript.len - at - len + 1);
    if (text.len) memcpy(t->transcript.data + at, text.data, text.len);
    t->transcript.len = t->transcript.len - len + text.len;
    size_t order = result ? c->end_order : c->start_order;
    for (tui_tool_call *it = t->tool_calls; it; it = it->next) {
        if (it->start_at != SIZE_MAX &&
            (it->start_at > at || (it->start_at == at && it->start_order > order)))
            it->start_at = it->start_at - len + text.len;
        if (it->end_at != SIZE_MAX &&
            (it->end_at > at || (it->end_at == at && it->end_order > order)))
            it->end_at = it->end_at - len + text.len;
    }
    if (t->scrollback_anchor > at)
        t->scrollback_anchor =
            t->scrollback_anchor < at + len ? at : t->scrollback_anchor - len + text.len;
    if (result) c->end_len = text.len;
    else c->start_len = text.len;
    t->transcript_generation++;
    t->transcript_version++;
    buf_free(&text);
}

void tui_tools_toggle(tui *t, tui_tool_call *c) {
    if (!c) return;
    c->pinned = true;
    c->expanded = !c->expanded;
    c->overridden = true;
    /* Inline terminal history cannot be rewritten. Its details viewer shares
     * this call state without mutating the printed row-to-call map. */
    if (t->fullscreen) {
        tui_transcript_commit(t);
        replace_card(t, c, true);
        replace_card(t, c, false);
    }
    c->pinned = false;
    t->tool_view_changed = true;
    t->dirty = true;
}

void tui_tools_default_changed(tui *t) {
    bool expanded = tny_settings_ui_tool_calls_expanded(t->ctx);
    tui_transcript_commit(t);
    for (tui_tool_call *c = t->tool_calls; c; c = c->next) {
        c->pinned = true;
        if (!c->overridden) c->expanded = expanded;
        if (t->fullscreen) {
            replace_card(t, c, true);
            replace_card(t, c, false);
        }
        c->pinned = false;
    }
    retain_tail(t);
    t->tool_view_changed = t->dirty = true;
}

void tui_tools_view_close(tui *t) {
    if (!t || !t->tool_view) return;
    t->tool_view = NULL;
    if (!t->fullscreen) tui_terminal_detail(false);
    fputs("\x1b[?25h", stdout);
    tui_terminal_mouse_update(t);
    t->dirty = true;
}

static void view_open(tui *t, tui_tool_call *c) {
    if (!c) {
        tui_note(t, "no retained tool calls");
        return;
    }
    if (!t->tty) {
        bool old = c->expanded;
        c->expanded = true;
        buf_t b = {0};
        heading(&b, c, true);
        buf_appends(&b, "Arguments:\n");
        tui_tool_literal(&b, c->args, false);
        if (c->done) {
            buf_appends(&b, "\nResult:\n");
            tui_tool_literal(&b, c->result, false);
        }
        buf_appends(&b, "\n");
        c->expanded = old;
        tui_write(t, b.data, b.len);
        buf_free(&b);
        return;
    }
    if (!t->tool_view && !t->fullscreen) tui_terminal_detail(true);
    t->tool_view = c;
    tui_terminal_mouse_update(t);
    t->tool_view_offset = 0;
    t->tool_view_changed = t->dirty = true;
}

void tui_tools_command(tui *t, const char *arg) {
    if (!t->tool_calls && t->session) {
        t->tool_restoring = true;
        yyjson_mut_val *messages = session_messages(t->session);
        size_t count = yyjson_mut_arr_size(messages);
        size_t limit = t->scrollback_lines ? t->scrollback_lines : TNY_UI_SCROLLBACK_LINES_DEFAULT;
        for (size_t i = count > limit ? count - limit : 0; i < count; i++)
            tui_tools_replay_message(t, yyjson_mut_arr_get(messages, i));
        t->tool_restoring = false;
    }
    tui_tool_call *c = t->tool_calls_tail;
    if (arg && *arg) {
        char *end = NULL;
        errno = 0;
        unsigned long long n = strtoull(arg, &end, 10);
        if (errno || !end || *end || !n) {
            tui_err(t, "usage: /tools [call number]");
            return;
        }
        for (c = t->tool_calls; c && c->number != n; c = c->next) {}
        if (!c) {
            tui_err(t, "tool call is not retained; use /transcript for session history");
            return;
        }
    }
    view_open(t, c);
}

bool tui_tools_key(tui *t, tui_key key) {
    if (!t->tool_view) return false;
    switch (key) {
    case TUI_K_ESC: tui_tools_view_close(t); break;
    case TUI_K_CTRLC:
    case TUI_K_CTRLD: tui_tools_view_close(t); return false;
    case TUI_K_ENTER: tui_tools_toggle(t, t->tool_view); break;
    case TUI_K_LEFT: {
        tui_tool_call *prev = NULL;
        for (tui_tool_call *c = t->tool_calls; c && c != t->tool_view; c = c->next) prev = c;
        if (prev) view_open(t, prev);
        break;
    }
    case TUI_K_RIGHT:
        if (t->tool_view->next) view_open(t, t->tool_view->next);
        break;
    case TUI_K_UP:
    case TUI_K_SCROLL_UP:
    case TUI_K_WHEEL_UP:
        if (t->tool_view_offset) t->tool_view_offset--;
        break;
    case TUI_K_DOWN:
    case TUI_K_SCROLL_DOWN:
    case TUI_K_WHEEL_DOWN: t->tool_view_offset++; break;
    case TUI_K_PAGE_UP:
        t->tool_view_offset =
            t->tool_view_offset > (size_t)t->rows ? t->tool_view_offset - (size_t)t->rows : 0;
        break;
    case TUI_K_PAGE_DOWN: t->tool_view_offset += (size_t)t->rows; break;
    case TUI_K_HOME:
    case TUI_K_SCROLL_HOME: t->tool_view_offset = 0; break;
    case TUI_K_END:
    case TUI_K_SCROLL_END: t->tool_view_offset = SIZE_MAX; break;
    default: break;
    }
    t->dirty = true;
    return true;
}

bool tui_tools_replay_message(tui *t, yyjson_mut_val *message) {
    const char *role = yyjson_mut_get_str(yyjson_mut_obj_get(message, "role"));
    if (!role) return false;
    if (strcmp(role, "assistant") == 0) {
        yyjson_mut_val *calls = yyjson_mut_obj_get(message, "tool_calls"), *call;
        size_t i, count;
        yyjson_mut_arr_foreach(calls, i, count, call) {
            if (!call) break;
            yyjson_mut_val *function = yyjson_mut_obj_get(call, "function");
            const char *args = yyjson_mut_get_str(yyjson_mut_obj_get(function, "arguments"));
            yyjson_doc *doc = args ? jparse(args, strlen(args)) : NULL;
            tny_backend_event event = {
                .kind = TNY_EV_TOOL_START,
                .tool_id = yyjson_mut_get_str(yyjson_mut_obj_get(call, "id")),
                .tool_name = yyjson_mut_get_str(yyjson_mut_obj_get(function, "name")),
                .tool_detail = args,
                .tool_reason = doc ? jget_str(yyjson_doc_get_root(doc), "reason") : NULL};
            tui_tools_event(t, &event);
            if (t->tool_calls_tail) t->tool_calls_tail->timed = false;
            yyjson_doc_free(doc);
        }
        return yyjson_mut_arr_size(calls) > 0;
    }
    if (strcmp(role, "tool") == 0) {
        const char *result = yyjson_mut_get_str(yyjson_mut_obj_get(message, "content"));
        tny_backend_event event = {
            .kind = TNY_EV_TOOL_END,
            .tool_id = yyjson_mut_get_str(yyjson_mut_obj_get(message, "tool_call_id")),
            .tool_detail = result,
            .tool_ok = !result || !str_starts(result, "error:")};
        tui_tools_event(t, &event);
        return true;
    }
    return false;
}
