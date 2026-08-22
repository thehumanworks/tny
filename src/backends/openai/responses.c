/* responses.c — Responses API wire translation (docs/adr/0016).
 *
 * Sessions persist Chat Completions-shaped messages (the lingua franca of
 * OpenAI-compatible providers, and what saved sessions already contain).
 * These pure functions translate that stored shape onto the Responses API
 * wire at request-build time: `input` items, flat function tools, and the
 * flattened `text.format` structured-output object. */
#include "backends/openai/openai.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

static const char *mstr(yyjson_mut_val *obj, const char *key) {
    return yyjson_mut_get_str(yyjson_mut_obj_get(obj, key));
}

static void add_str(yyjson_mut_doc *d, yyjson_mut_val *obj,
                    const char *k, const char *v) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(d, k), yyjson_mut_strcpy(d, v));
}

/* {"role":R,"content":"text"} — EasyInputMessage with string content. */
static void add_text_message(yyjson_mut_doc *d, yyjson_mut_val *arr,
                             const char *role, const char *text) {
    yyjson_mut_val *m = yyjson_mut_obj(d);
    add_str(d, m, "role", role);
    add_str(d, m, "content", text);
    yyjson_mut_arr_add_val(arr, m);
}

/* Chat image message (content parts text / image_url{url}) → responses
 * message with input_text / input_image parts. Unknown parts are skipped. */
static void add_parts_message(yyjson_mut_doc *d, yyjson_mut_val *arr,
                              const char *role, yyjson_mut_val *parts) {
    yyjson_mut_val *m = yyjson_mut_obj(d);
    add_str(d, m, "role", role);
    yyjson_mut_val *out = yyjson_mut_arr(d);
    size_t idx, max;
    yyjson_mut_val *p;
    yyjson_mut_arr_foreach(parts, idx, max, p) {
        const char *type = mstr(p, "type");
        if (type && strcmp(type, "text") == 0) {
            const char *t = mstr(p, "text");
            yyjson_mut_val *np = yyjson_mut_obj(d);
            add_str(d, np, "type", "input_text");
            add_str(d, np, "text", t ? t : "");
            yyjson_mut_arr_add_val(out, np);
        } else if (type && strcmp(type, "image_url") == 0) {
            yyjson_mut_val *iu = yyjson_mut_obj_get(p, "image_url");
            const char *url = mstr(iu, "url");
            if (!url) continue;
            yyjson_mut_val *np = yyjson_mut_obj(d);
            add_str(d, np, "type", "input_image");
            add_str(d, np, "image_url", url);
            yyjson_mut_arr_add_val(out, np);
        }
    }
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(d, "content"), out);
    yyjson_mut_arr_add_val(arr, m);
}

/* Assistant tool_calls → one function_call item per call. */
static void add_function_calls(yyjson_mut_doc *d, yyjson_mut_val *arr,
                               yyjson_mut_val *tcs) {
    size_t idx, max;
    yyjson_mut_val *tc;
    yyjson_mut_arr_foreach(tcs, idx, max, tc) {
        yyjson_mut_val *fn = yyjson_mut_obj_get(tc, "function");
        const char *id = mstr(tc, "id");
        const char *name = fn ? mstr(fn, "name") : NULL;
        const char *args = fn ? mstr(fn, "arguments") : NULL;
        yyjson_mut_val *item = yyjson_mut_obj(d);
        add_str(d, item, "type", "function_call");
        add_str(d, item, "call_id", id ? id : "call_0");
        add_str(d, item, "name", name ? name : "unknown");
        add_str(d, item, "arguments", args ? args : "{}");
        yyjson_mut_arr_add_val(arr, item);
    }
}

char *tny_openai_responses_input(yyjson_mut_val *msgs, int boundary,
                                 const char *summary) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) return NULL;
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, arr);
    if (summary && boundary > 0)
        add_text_message(d, arr, "system", summary);

    size_t total = msgs ? yyjson_mut_arr_size(msgs) : 0;
    for (size_t i = boundary > 0 ? (size_t)boundary : 0; i < total; i++) {
        yyjson_mut_val *m = yyjson_mut_arr_get(msgs, i);
        const char *role = mstr(m, "role");
        if (!role) continue;
        yyjson_mut_val *content = yyjson_mut_obj_get(m, "content");
        if (strcmp(role, "tool") == 0) {
            const char *id = mstr(m, "tool_call_id");
            const char *out = yyjson_mut_get_str(content);
            yyjson_mut_val *item = yyjson_mut_obj(d);
            add_str(d, item, "type", "function_call_output");
            add_str(d, item, "call_id", id ? id : "call_0");
            add_str(d, item, "output", out ? out : "");
            yyjson_mut_arr_add_val(arr, item);
            continue;
        }
        if (strcmp(role, "assistant") == 0) {
            const char *text = yyjson_mut_get_str(content);
            if (text && *text) add_text_message(d, arr, "assistant", text);
            yyjson_mut_val *tcs = yyjson_mut_obj_get(m, "tool_calls");
            if (tcs && yyjson_mut_is_arr(tcs)) add_function_calls(d, arr, tcs);
            continue;
        }
        /* user / system / developer */
        if (yyjson_mut_is_arr(content)) {
            add_parts_message(d, arr, role, content);
        } else {
            const char *text = yyjson_mut_get_str(content);
            add_text_message(d, arr, role, text ? text : "");
        }
    }
    char *out = jwrite(d);
    yyjson_mut_doc_free(d);
    return out;
}

char *tny_openai_responses_tools(const char *chat_tools_json) {
    if (!chat_tools_json) return NULL;
    yyjson_doc *doc = jparse(chat_tools_json, strlen(chat_tools_json));
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) { yyjson_doc_free(doc); return NULL; }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) { yyjson_doc_free(doc); return NULL; }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, arr);
    size_t idx, max;
    yyjson_val *t;
    yyjson_arr_foreach(root, idx, max, t) {
        yyjson_val *fn = jget(t, "function");
        yyjson_mut_val *item = yyjson_mut_obj(d);
        add_str(d, item, "type", "function");
        if (fn) {
            /* flatten: name/description/parameters ride at the top level */
            size_t fi, fm;
            yyjson_val *k, *v;
            yyjson_obj_foreach(fn, fi, fm, k, v) {
                yyjson_mut_val *cv = yyjson_val_mut_copy(d, v);
                if (cv)
                    yyjson_mut_obj_put(item,
                                       yyjson_mut_strcpy(d, yyjson_get_str(k)), cv);
            }
        }
        yyjson_mut_arr_add_val(arr, item);
    }
    char *out = jwrite(d);
    yyjson_mut_doc_free(d);
    yyjson_doc_free(doc);
    return out;
}

char *tny_openai_responses_text_format(const char *response_format_json) {
    if (!response_format_json) return NULL;
    yyjson_doc *doc = jparse(response_format_json, strlen(response_format_json));
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *type = jget_str(root, "type");
    if (!type || strcmp(type, "json_schema") != 0) {
        yyjson_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) { yyjson_doc_free(doc); return NULL; }
    yyjson_mut_val *fmt = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, fmt);
    add_str(d, fmt, "type", "json_schema");
    /* chat nests under "json_schema"; responses flattens its members */
    yyjson_val *js = jget(root, "json_schema");
    if (js && yyjson_is_obj(js)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(js, idx, max, k, v) {
            yyjson_mut_val *cv = yyjson_val_mut_copy(d, v);
            if (cv)
                yyjson_mut_obj_put(fmt, yyjson_mut_strcpy(d, yyjson_get_str(k)), cv);
        }
    }
    char *out = jwrite(d);
    yyjson_mut_doc_free(d);
    yyjson_doc_free(doc);
    return out;
}
