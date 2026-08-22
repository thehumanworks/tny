/* toolcalls.c — assembly of streamed Chat Completions tool_call fragments
 * into whole calls (docs/backends/openai-compatible.md).
 *
 * The spec shape keys fragments by "index": the first fragment of a call
 * carries id + function.name, later ones only index + argument pieces.
 * Real traffic also contains gateway shapes that repeat or omit the index
 * while giving each call its own id. Keying strictly by index merged such
 * calls: one call's id vanished and its arguments were glued onto the
 * previous call, so the next request was missing a tool output and strict
 * providers rejected the whole session with HTTP 400 ("no tool output
 * found for function call …"). Attribution is id-first to make a fresh id
 * always open a fresh call. */
#include "backends/openai/openai.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static oa_call *calls_new(oa_callset *cs, int wire_index) {
    if (cs->n >= OA_MAX_TOOL_CALLS) return NULL;
    oa_call *pc = &cs->calls[cs->n++];
    pc->id = NULL;
    pc->name = NULL;
    buf_init(&pc->args);
    pc->wire_index = wire_index;
    return pc;
}

static oa_call *calls_by_id(oa_callset *cs, const char *id) {
    for (int i = 0; i < cs->n; i++)
        if (cs->calls[i].id && strcmp(cs->calls[i].id, id) == 0)
            return &cs->calls[i];
    return NULL;
}

/* Most recent call for a wire index: a buggy gateway can reuse an index
 * for a second call, and continuation fragments belong to the newer one. */
static oa_call *calls_by_index(oa_callset *cs, int wire_index) {
    for (int i = cs->n - 1; i >= 0; i--)
        if (cs->calls[i].wire_index == wire_index)
            return &cs->calls[i];
    return NULL;
}

void oa_calls_feed(oa_callset *cs, yyjson_val *tool_calls) {
    if (!tool_calls || !yyjson_is_arr(tool_calls)) return;
    size_t ai, amax;
    yyjson_val *tc;
    yyjson_arr_foreach(tool_calls, ai, amax, tc) {
        const char *id = jget_str(tc, "id");
        if (id && !*id) id = NULL;
        bool has_index = jget(tc, "index") != NULL;
        int wire_index = (int)jget_int(tc, "index", -1);
        if (has_index && wire_index < 0) continue; /* nonsense index */

        oa_call *pc = NULL;
        if (id) {
            pc = calls_by_id(cs, id);
            if (!pc && has_index) {
                /* fragments for this index arrived before the id did */
                oa_call *q = calls_by_index(cs, wire_index);
                if (q && !q->id) {
                    pc = q;
                    pc->id = xstrdup(id);
                }
            }
            if (!pc) {
                /* unseen id: a new call starts here even when the provider
                 * repeated or omitted "index" */
                pc = calls_new(cs, has_index ? wire_index : cs->n);
                if (pc) pc->id = xstrdup(id);
            }
        } else if (has_index) {
            pc = calls_by_index(cs, wire_index);
            if (!pc) pc = calls_new(cs, wire_index);
        } else if (cs->n > 0) {
            pc = &cs->calls[cs->n - 1];
        }
        if (!pc) continue; /* overflow or an orphan fragment: drop it */

        yyjson_val *fn = jget(tc, "function");
        const char *name = jget_str(fn, "name");
        if (name && !pc->name) pc->name = xstrdup(name);
        const char *frag = jget_str(fn, "arguments");
        if (frag) buf_appends(&pc->args, frag);
    }
}

void oa_calls_reset(oa_callset *cs) {
    for (int i = 0; i < cs->n; i++) {
        free(cs->calls[i].id);
        free(cs->calls[i].name);
        buf_free(&cs->calls[i].args);
    }
    cs->n = 0;
}

const char *oa_call_id(const oa_call *pc, int slot, char *buf, size_t buflen) {
    if (pc->id) return pc->id;
    snprintf(buf, buflen, "call_%d", slot);
    return buf;
}
