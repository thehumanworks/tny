#include "core/cursor_config.h"
#include "util/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return -1;
}

static bool credential_key(const char *key) {
    char normalized[64];
    size_t n = 0;
    for (; key && *key && n + 1 < sizeof normalized; key++) {
        unsigned char c = (unsigned char)*key;
        if (isalnum(c)) normalized[n++] = (char)tolower(c);
    }
    normalized[n] = '\0';
    return strcmp(normalized, "apikey") == 0 || strcmp(normalized, "clientsecret") == 0 ||
           strcmp(normalized, "authtoken") == 0 || strcmp(normalized, "accesstoken") == 0 ||
           strcmp(normalized, "refreshtoken") == 0 || strcmp(normalized, "password") == 0;
}

static bool contains_credential(yyjson_val *v) {
    if (yyjson_is_obj(v)) {
        size_t idx, max;
        yyjson_val *k, *child;
        yyjson_obj_foreach(v, idx, max, k, child) {
            if (credential_key(yyjson_get_str(k)) || contains_credential(child)) return true;
        }
    } else if (yyjson_is_arr(v)) {
        size_t idx, max;
        yyjson_val *child;
        yyjson_arr_foreach(v, idx, max, child) {
            if (contains_credential(child)) return true;
        }
    }
    return false;
}

static int require_object(yyjson_val *parent, const char *key, const char *where, char *err,
                          size_t errlen) {
    yyjson_val *v = jget(parent, key);
    if (!v || yyjson_is_obj(v)) return 0;
    char msg[160];
    snprintf(msg, sizeof msg, "%s.%s must be an object", where, key);
    return fail(err, errlen, msg);
}

static int require_string_map(yyjson_val *parent, const char *key, const char *where, char *err,
                              size_t errlen) {
    yyjson_val *map = jget(parent, key);
    if (!map) return 0;
    if (!yyjson_is_obj(map)) return require_object(parent, key, where, err, errlen);
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(map, idx, max, k, v) {
        (void)k;
        if (!yyjson_is_str(v)) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s.%s values must be strings", where, key);
            return fail(err, errlen, msg);
        }
    }
    return 0;
}

static int require_object_map(yyjson_val *parent, const char *key, const char *where, char *err,
                              size_t errlen) {
    yyjson_val *map = jget(parent, key);
    if (!map) return 0;
    if (!yyjson_is_obj(map)) return require_object(parent, key, where, err, errlen);
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(map, idx, max, k, v) {
        (void)k;
        if (!yyjson_is_obj(v)) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s.%s values must be objects", where, key);
            return fail(err, errlen, msg);
        }
    }
    return 0;
}

static int validate_mode(yyjson_val *parent, const char *where, char *err, size_t errlen) {
    yyjson_val *v = jget(parent, "mode");
    if (!v) return 0;
    if (yyjson_is_uint(v) && yyjson_get_uint(v) <= 2) return 0;
    const char *s = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
    if (s &&
        (strcmp(s, "AGENT_MODE_OPTION_UNSPECIFIED") == 0 ||
         strcmp(s, "AGENT_MODE_OPTION_AGENT") == 0 || strcmp(s, "AGENT_MODE_OPTION_PLAN") == 0))
        return 0;
    char msg[192];
    snprintf(msg, sizeof msg, "%s.mode must be an AGENT_MODE_OPTION_* protojson enum (or 0..2)",
             where);
    return fail(err, errlen, msg);
}

static int validate_store(yyjson_val *store, const char *where, char *err, size_t errlen) {
    if (!store) return 0;
    if (!yyjson_is_obj(store)) {
        char msg[128];
        snprintf(msg, sizeof msg, "%s must be an object", where);
        return fail(err, errlen, msg);
    }
    yyjson_val *tv = jget(store, "type");
    const char *type = yyjson_is_str(tv) ? yyjson_get_str(tv) : NULL;
    if (!type || (strcmp(type, "sqlite") != 0 && strcmp(type, "jsonl") != 0 &&
                  strcmp(type, "custom") != 0)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s.type must be sqlite, jsonl, or custom", where);
        return fail(err, errlen, msg);
    }
    yyjson_val *root = jget(store, "rootDir");
    if (!root) root = jget(store, "root_dir");
    if (strcmp(type, "jsonl") == 0) {
        if (!yyjson_is_str(root) || yyjson_get_len(root) == 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s.rootDir is required for jsonl", where);
            return fail(err, errlen, msg);
        }
    } else if (root) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s.rootDir is valid only for jsonl", where);
        return fail(err, errlen, msg);
    }
    return 0;
}

static int validate_string_array(yyjson_val *parent, const char *key, const char *where,
                                 size_t max_items, char *err, size_t errlen) {
    yyjson_val *a = jget(parent, key);
    if (!a) return 0;
    if (!yyjson_is_arr(a) || (max_items && yyjson_arr_size(a) > max_items)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s.%s must be a string array%s", where, key,
                 max_items == 1 ? " with at most one value" : "");
        return fail(err, errlen, msg);
    }
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(a, idx, max, v) {
        if (!yyjson_is_str(v) || yyjson_get_len(v) == 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s.%s values must be nonempty strings", where, key);
            return fail(err, errlen, msg);
        }
    }
    return 0;
}

static int validate_custom_tools(yyjson_val *local, char *err, size_t errlen) {
    if (require_object_map(local, "customTools", "cursor.agent_options.local", err, errlen) != 0)
        return -1;
    yyjson_val *map = jget(local, "customTools");
    if (!map) map = jget(local, "custom_tools");
    if (!map) return 0;
    if (!yyjson_is_obj(map))
        return fail(err, errlen, "cursor.agent_options.local.customTools must be an object");
    size_t idx, max;
    yyjson_val *k, *tool;
    yyjson_obj_foreach(map, idx, max, k, tool) {
        (void)k;
        if (!yyjson_is_obj(tool))
            return fail(err, errlen,
                        "cursor.agent_options.local.customTools values must be objects");
        yyjson_val *input = jget(tool, "inputSchema");
        if (!input) input = jget(tool, "input_schema");
        yyjson_val *output = jget(tool, "outputSchema");
        if (!output) output = jget(tool, "output_schema");
        if ((input && !yyjson_is_obj(input)) || (output && !yyjson_is_obj(output)))
            return fail(err, errlen, "Cursor custom-tool schemas must be JSON objects");
    }
    return 0;
}

static int validate_model(yyjson_val *parent, const char *where, char *err, size_t errlen) {
    yyjson_val *model = jget(parent, "model");
    if (!model) return 0;
    if (!yyjson_is_obj(model)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s.model must be an object", where);
        return fail(err, errlen, msg);
    }
    yyjson_val *params = jget(model, "params");
    if (!params) return 0;
    if (!yyjson_is_arr(params)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s.model.params must be an array", where);
        return fail(err, errlen, msg);
    }
    size_t idx, max;
    yyjson_val *param;
    yyjson_arr_foreach(params, idx, max, param) {
        if (!yyjson_is_obj(param) || !yyjson_is_str(jget(param, "id")) ||
            !yyjson_is_str(jget(param, "value"))) {
            char msg[192];
            snprintf(msg, sizeof msg, "%s.model.params values must contain string id/value", where);
            return fail(err, errlen, msg);
        }
    }
    return 0;
}

static int validate_agent_options(yyjson_val *o, tny_cursor_runtime runtime, char *err,
                                  size_t errlen) {
    if (!o) return 0;
    if (!yyjson_is_obj(o)) return fail(err, errlen, "cursor.agent_options must be an object");
    yyjson_val *local = jget(o, "local"), *cloud = jget(o, "cloud");
    if ((local && !yyjson_is_obj(local)) || (cloud && !yyjson_is_obj(cloud)))
        return fail(err, errlen, "cursor agent local/cloud options must be objects");
    if (local && cloud)
        return fail(err, errlen, "cursor.agent_options cannot combine local and cloud");
    if ((runtime == TNY_CURSOR_RUNTIME_LOCAL && cloud) ||
        (runtime == TNY_CURSOR_RUNTIME_CLOUD && local))
        return fail(err, errlen, "cursor runtime conflicts with agent_options");
    if (validate_model(o, "cursor.agent_options", err, errlen) != 0 ||
        validate_mode(o, "cursor.agent_options", err, errlen) != 0 ||
        require_object_map(o, "mcpServers", "cursor.agent_options", err, errlen) != 0 ||
        require_object_map(o, "agents", "cursor.agent_options", err, errlen) != 0)
        return -1;
    yyjson_val *tools = jget(o, "tools");
    if (tools &&
        (!yyjson_is_obj(tools) ||
         validate_string_array(tools, "names", "cursor.agent_options.tools", 0, err, errlen) != 0))
        return fail(err, errlen, "cursor.agent_options.tools must be {\"names\":[...]}");
    if (validate_string_array(o, "disallowedTools", "cursor.agent_options", 0, err, errlen) != 0)
        return -1;
    if (local) {
        if (validate_string_array(local, "cwd", "cursor.agent_options.local", 1, err, errlen) !=
                0 ||
            validate_string_array(local, "dirs", "cursor.agent_options.local", 0, err, errlen) !=
                0 ||
            validate_store(jget(local, "store"), "cursor.agent_options.local.store", err, errlen) !=
                0 ||
            validate_custom_tools(local, err, errlen) != 0)
            return -1;
    }
    if (cloud &&
        (require_string_map(cloud, "envVars", "cursor.agent_options.cloud", err, errlen) != 0 ||
         require_string_map(cloud, "metadata", "cursor.agent_options.cloud", err, errlen) != 0))
        return -1;
    return 0;
}

static int validate_send_options(yyjson_val *o, tny_cursor_runtime runtime, char *err,
                                 size_t errlen) {
    if (!o) return 0;
    if (!yyjson_is_obj(o)) return fail(err, errlen, "cursor.send_options must be an object");
    yyjson_val *local = jget(o, "local"), *cloud = jget(o, "cloud");
    if ((local && !yyjson_is_obj(local)) || (cloud && !yyjson_is_obj(cloud)))
        return fail(err, errlen, "cursor send local/cloud options must be objects");
    if (local && cloud)
        return fail(err, errlen, "cursor.send_options cannot combine local and cloud");
    if ((runtime == TNY_CURSOR_RUNTIME_LOCAL && cloud) ||
        (runtime == TNY_CURSOR_RUNTIME_CLOUD && local))
        return fail(err, errlen, "cursor runtime conflicts with send_options");
    if (validate_model(o, "cursor.send_options", err, errlen) != 0 ||
        validate_mode(o, "cursor.send_options", err, errlen) != 0 ||
        require_object_map(o, "mcpServers", "cursor.send_options", err, errlen) != 0)
        return -1;
    if (cloud && require_string_map(cloud, "envVars", "cursor.send_options.cloud", err, errlen))
        return -1;
    return 0;
}

static char *copy_json(yyjson_val *v) { return v ? jwrite_val(v) : xstrdup("{}"); }

tny_cursor_config *tny_cursor_config_load(yyjson_val *settings_cursor, yyjson_val *repo_cursor,
                                          char *err, size_t errlen) {
    if (repo_cursor && contains_credential(repo_cursor)) {
        fail(err, errlen, "credentials are forbidden in repo .tny.json cursor config");
        return NULL;
    }
    if (settings_cursor && !yyjson_is_obj(settings_cursor)) {
        fail(err, errlen, "settings.json cursor must be an object");
        return NULL;
    }
    tny_cursor_config *cfg = calloc(1, sizeof *cfg);
    if (!cfg) return NULL;
    cfg->runtime = TNY_CURSOR_RUNTIME_AUTO;
    cfg->tool_callbacks = true;
    cfg->store_callbacks = true;

    const char *runtime = jget_str(settings_cursor, "runtime");
    if (runtime) {
        if (strcmp(runtime, "auto") == 0) cfg->runtime = TNY_CURSOR_RUNTIME_AUTO;
        else if (strcmp(runtime, "local") == 0) cfg->runtime = TNY_CURSOR_RUNTIME_LOCAL;
        else if (strcmp(runtime, "cloud") == 0) cfg->runtime = TNY_CURSOR_RUNTIME_CLOUD;
        else {
            fail(err, errlen, "settings.json cursor.runtime must be auto, local, or cloud");
            goto bad;
        }
    }
    yyjson_val *state = jget(settings_cursor, "state_root");
    if (state && (!yyjson_is_str(state) || yyjson_get_len(state) == 0)) {
        fail(err, errlen, "settings.json cursor.state_root must be a nonempty string");
        goto bad;
    }
    if (state) cfg->state_root = xstrdup(yyjson_get_str(state));
    yyjson_val *store = jget(settings_cursor, "local_store");
    if (validate_store(store, "settings.json cursor.local_store", err, errlen) != 0) goto bad;
    if (store) cfg->local_store_json = jwrite_val(store);

    yyjson_val *callbacks = jget(settings_cursor, "callbacks");
    if (callbacks && !yyjson_is_obj(callbacks)) {
        fail(err, errlen, "settings.json cursor.callbacks must be an object");
        goto bad;
    }
    yyjson_val *tool_cb = jget(callbacks, "custom_tools");
    yyjson_val *store_cb = jget(callbacks, "store");
    if ((tool_cb && !yyjson_is_bool(tool_cb)) || (store_cb && !yyjson_is_bool(store_cb))) {
        fail(err, errlen, "Cursor callback enablement values must be booleans");
        goto bad;
    }
    if (tool_cb) cfg->tool_callbacks = yyjson_get_bool(tool_cb);
    if (store_cb) cfg->store_callbacks = yyjson_get_bool(store_cb);

    yyjson_val *agent = jget(settings_cursor, "agent_options");
    yyjson_val *send = jget(settings_cursor, "send_options");
    if (validate_agent_options(agent, cfg->runtime, err, errlen) != 0 ||
        validate_send_options(send, cfg->runtime, err, errlen) != 0)
        goto bad;
    cfg->agent_options_json = copy_json(agent);
    cfg->send_options_json = copy_json(send);
    if (!cfg->agent_options_json || !cfg->send_options_json || (state && !cfg->state_root) ||
        (store && !cfg->local_store_json))
        goto bad;
    return cfg;
bad:
    tny_cursor_config_free(cfg);
    return NULL;
}

void tny_cursor_config_free(tny_cursor_config *cfg) {
    if (!cfg) return;
    free(cfg->state_root);
    free(cfg->local_store_json);
    /* Protojson pass-throughs may contain MCP auth or environment secrets. */
    secure_free(cfg->agent_options_json);
    secure_free(cfg->send_options_json);
    free(cfg);
}
