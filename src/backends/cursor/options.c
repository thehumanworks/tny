#include "backends/cursor/options.h"
#include "core/config.h"
#include "core/cursor_config.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return NULL;
}

static yyjson_mut_doc *mutable_object(const char *json, char *err, size_t errlen) {
    const char *source = json ? json : "{}";
    yyjson_doc *parsed = jparse(source, strlen(source));
    yyjson_val *root = parsed ? yyjson_doc_get_root(parsed) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(parsed);
        fail(err, errlen, "Cursor options must encode a JSON object");
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_doc_mut_copy(parsed, jallocator());
    yyjson_doc_free(parsed);
    if (!doc) fail(err, errlen, "could not allocate Cursor options");
    return doc;
}

static void put_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, const char *value) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_strcpy(doc, value));
}

static void inject_model(const tny_ctx *ctx, yyjson_mut_doc *doc, yyjson_mut_val *root) {
    if (!ctx || !ctx->model || !*ctx->model) return;
    yyjson_mut_val *model = yyjson_mut_obj_get(root, "model");
    if (!yyjson_mut_is_obj(model)) {
        model = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "model"), model);
    }
    put_str(doc, model, "id", ctx->model);
}

static yyjson_mut_val *string_array(yyjson_mut_doc *doc, char **values, int n) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < n; i++) yyjson_mut_arr_add_strcpy(doc, arr, values[i]);
    return arr;
}

static bool valid_custom_tools(yyjson_val *root) {
    if (!yyjson_is_obj(root)) return false;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(root, idx, max, k, v) {
        (void)k;
        if (!yyjson_is_obj(v)) return false;
        yyjson_val *input = jget(v, "inputSchema");
        if (!input) input = jget(v, "input_schema");
        yyjson_val *output = jget(v, "outputSchema");
        if (!output) output = jget(v, "output_schema");
        if ((input && !yyjson_is_obj(input)) || (output && !yyjson_is_obj(output))) return false;
    }
    return true;
}

char *cursor_options_agent_json(const tny_ctx *ctx, const char *api_key,
                                const char *custom_tools_json, char *err, size_t errlen) {
    if (!ctx || !ctx->cursor_config) return fail(err, errlen, "Cursor config is unavailable");
    if (!api_key || !*api_key) return fail(err, errlen, "CURSOR_API_KEY is required");
    const tny_cursor_config *cfg = ctx->cursor_config;
    yyjson_mut_doc *doc = mutable_object(cfg->agent_options_json, err, errlen);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);

    /* Never trust persisted key material, including legacy snake_case. */
    yyjson_mut_obj_remove_key(root, "apiKey");
    yyjson_mut_obj_remove_key(root, "api_key");
    put_str(doc, root, "apiKey", api_key);
    inject_model(ctx, doc, root);

    yyjson_mut_val *local = yyjson_mut_obj_get(root, "local");
    yyjson_mut_val *cloud = yyjson_mut_obj_get(root, "cloud");
    if (!local && !cloud) {
        const char *kind = cfg->runtime == TNY_CURSOR_RUNTIME_CLOUD ? "cloud" : "local";
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, kind), yyjson_mut_obj(doc));
        local = yyjson_mut_obj_get(root, "local");
    }
    if (yyjson_mut_is_obj(local)) {
        if (!yyjson_mut_obj_get(local, "cwd")) {
            char *cwdv[1] = {ctx->cwd};
            yyjson_mut_obj_put(local, yyjson_mut_strcpy(doc, "cwd"), string_array(doc, cwdv, 1));
        }
        if (!yyjson_mut_obj_get(local, "dirs") && ctx->n_extra_dirs > 0)
            yyjson_mut_obj_put(local, yyjson_mut_strcpy(doc, "dirs"),
                               string_array(doc, ctx->extra_dirs, ctx->n_extra_dirs));
    }

    if (custom_tools_json) {
        yyjson_doc *tools_doc = jparse(custom_tools_json, strlen(custom_tools_json));
        yyjson_val *tools = tools_doc ? yyjson_doc_get_root(tools_doc) : NULL;
        if (!valid_custom_tools(tools)) {
            yyjson_doc_free(tools_doc);
            yyjson_mut_doc_free(doc);
            return fail(
                err, errlen,
                "registered Cursor custom tools must be an object map of object definitions");
        }
        if (!yyjson_mut_is_obj(local)) {
            yyjson_doc_free(tools_doc);
            yyjson_mut_doc_free(doc);
            return fail(err, errlen, "Cursor custom tools require the local runtime");
        }
        yyjson_mut_val *copy = yyjson_val_mut_copy(doc, tools);
        yyjson_doc_free(tools_doc);
        if (!copy) {
            yyjson_mut_doc_free(doc);
            return fail(err, errlen, "could not allocate Cursor custom-tool metadata");
        }
        yyjson_mut_obj_remove_key(local, "custom_tools");
        yyjson_mut_obj_put(local, yyjson_mut_strcpy(doc, "customTools"), copy);
    }

    char *out = jwrite(doc);
    yyjson_mut_doc_free(doc);
    if (!out) return fail(err, errlen, "could not serialize Cursor AgentOptions");
    return out;
}

char *cursor_options_send_json(const tny_ctx *ctx, bool interactive_stream, char *err,
                               size_t errlen) {
    if (!ctx || !ctx->cursor_config) return fail(err, errlen, "Cursor config is unavailable");
    yyjson_mut_doc *doc = mutable_object(ctx->cursor_config->send_options_json, err, errlen);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    inject_model(ctx, doc, root);
    if (interactive_stream)
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "enableDeltas"), yyjson_mut_true(doc));
    char *out = jwrite(doc);
    yyjson_mut_doc_free(doc);
    if (!out) return fail(err, errlen, "could not serialize Cursor SendOptions");
    return out;
}
