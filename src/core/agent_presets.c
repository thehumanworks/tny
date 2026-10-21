#include "core/agent_presets.h"
#include "core/tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yyjson_val *preset_map(const tny_ctx *ctx) {
    return ctx && ctx->settings ? jget(yyjson_doc_get_root(ctx->settings), "agent_presets") : NULL;
}

static bool bounded_text(yyjson_val *value, bool empty_allowed) {
    const char *text = yyjson_get_str(value);
    size_t len = yyjson_get_len(value);
    return text && (empty_allowed || len) && len <= TNY_AGENT_PRESET_TEXT_MAX &&
           utf8_valid_bytes(text, len);
}

static bool valid_name(yyjson_val *value) {
    const char *name = yyjson_get_str(value);
    return name && strlen(name) == yyjson_get_len(value) && tny_task_name_valid(name);
}

static bool valid_result_cap(yyjson_val *value) {
    if (!yyjson_is_int(value)) return false;
    if (yyjson_is_sint(value) && yyjson_get_sint(value) < 0) return false;
    uint64_t cap = yyjson_get_uint(value);
    return cap >= TNY_AGENT_PRESET_RESULT_MIN && cap <= TNY_AGENT_PRESET_RESULT_MAX;
}

/* The returned text contains only static field names, never user content. */
static const char *entry_error(yyjson_val *entry, bool snapshot) {
    if (!yyjson_is_obj(entry)) return "must be an object";
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(entry, i, count, key, value) {
        const char *name = yyjson_get_str(key);
        size_t len = yyjson_get_len(key);
        if (!name || strlen(name) != len || yyjson_obj_getn(entry, name, len) != value)
            return "must not contain duplicate or NUL-containing fields";
        if (strcmp(name, "provider") == 0 || strcmp(name, "model") == 0 ||
            strcmp(name, "effort") == 0) {
            if (!bounded_text(value, false))
                return "provider, model and effort must be nonempty NUL-free UTF-8 strings "
                       "of at most 256 bytes";
        } else if (strcmp(name, "task") == 0 || (snapshot && strcmp(name, "name") == 0)) {
            if (!valid_name(value)) return "name and task must be valid task names";
        } else if (strcmp(name, "description") == 0) {
            if (!bounded_text(value, true))
                return "description must be a NUL-free UTF-8 string of at most 256 bytes";
        } else if (strcmp(name, "max_result_bytes") == 0) {
            if (!valid_result_cap(value))
                return "max_result_bytes must be an integer from 1024 to 65536";
        } else {
            return "contains an unsupported field; only provider, model, effort, task, "
                   "description and max_result_bytes are allowed";
        }
    }
    if (!jget(entry, "provider") || !jget(entry, "model"))
        return "requires explicit provider and model";
    if (snapshot && (!jget(entry, "name") || !jget(entry, "task") || !jget(entry, "description") ||
                     !jget(entry, "max_result_bytes")))
        return "is missing canonical snapshot fields";
    return NULL;
}

int tny_agent_presets_validate(const tny_ctx *ctx) {
    yyjson_val *root = ctx && ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    if (!yyjson_is_obj(root)) return 0;
    yyjson_val *map = jget(root, "agent_presets");
    if (!map) return 0;
    size_t i, count;
    yyjson_val *key, *entry;
    yyjson_obj_foreach(root, i, count, key, entry) {
        if (yyjson_equals_str(key, "agent_presets") && entry != map) {
            fputs("tny: settings.json agent_presets must not be defined more than once\n", stderr);
            return -1;
        }
    }
    if (!yyjson_is_obj(map) || yyjson_obj_size(map) > TNY_AGENT_PRESET_COUNT_MAX) {
        fputs("tny: settings.json agent_presets must be an object with at most 32 entries\n",
              stderr);
        return -1;
    }
    yyjson_obj_foreach(map, i, count, key, entry) {
        const char *name = yyjson_get_str(key);
        if (!valid_name(key)) {
            fputs("tny: settings.json agent_presets keys must be valid task names\n", stderr);
            return -1;
        }
        if (yyjson_obj_get(map, name) != entry) {
            fputs("tny: settings.json agent_presets must not contain duplicate names\n", stderr);
            return -1;
        }
        const char *error = entry_error(entry, false);
        if (error) {
            fprintf(stderr, "tny: settings.json agent_presets entry %zu %s\n", i + 1, error);
            return -1;
        }
    }
    return 0;
}

bool tny_agent_preset_snapshot_valid(yyjson_val *snapshot) {
    return entry_error(snapshot, true) == NULL;
}

char *tny_agent_preset_resolve(const tny_ctx *ctx, const char *name) {
    if (!tny_task_name_valid(name)) {
        fputs("tny: agent preset selection must be a valid task name\n", stderr);
        return NULL;
    }
    if (tny_agent_presets_validate(ctx) != 0) return NULL;
    yyjson_val *entry = jget(preset_map(ctx), name);
    if (!entry) {
        fputs("tny: requested agent preset is not configured in settings.json agent_presets\n",
              stderr);
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    const char *effort = jget_str(entry, "effort");
    const char *task = jget_str(entry, "task");
    const char *description = jget_str(entry, "description");
    char *result = NULL;
    if (root && yyjson_mut_obj_add_strcpy(doc, root, "name", name) &&
        yyjson_mut_obj_add_strcpy(doc, root, "provider", jget_str(entry, "provider")) &&
        yyjson_mut_obj_add_strcpy(doc, root, "model", jget_str(entry, "model")) &&
        (!effort || yyjson_mut_obj_add_strcpy(doc, root, "effort", effort)) &&
        yyjson_mut_obj_add_strcpy(doc, root, "task", task ? task : TNY_AGENT_PRESET_TASK_DEFAULT) &&
        yyjson_mut_obj_add_strcpy(doc, root, "description", description ? description : "") &&
        yyjson_mut_obj_add_uint(
            doc, root, "max_result_bytes",
            (uint64_t)jget_int(entry, "max_result_bytes", TNY_AGENT_PRESET_RESULT_DEFAULT))) {
        yyjson_mut_doc_set_root(doc, root);
        result = jwrite(doc);
    }
    yyjson_mut_doc_free(doc);
    if (!result) fputs("tny: out of memory resolving agent preset\n", stderr);
    return result;
}

typedef struct {
    const char *name;
    const char *description;
} preset_info;

static int compare_presets(const void *lhs, const void *rhs) {
    const preset_info *a = lhs, *b = rhs;
    return strcmp(a->name, b->name);
}

void tny_agent_presets_collect(const tny_ctx *ctx, buf_t *out) {
    if (!out || tny_agent_presets_validate(ctx) != 0) return;
    yyjson_val *map = preset_map(ctx);
    if (!yyjson_obj_size(map)) return;
    preset_info entries[TNY_AGENT_PRESET_COUNT_MAX];
    size_t i, count;
    yyjson_val *key, *entry;
    yyjson_obj_foreach(map, i, count, key, entry) {
        entries[i].name = yyjson_get_str(key);
        entries[i].description = jget_str(entry, "description");
    }
    count = yyjson_obj_size(map);
    qsort(entries, count, sizeof *entries, compare_presets);
    buf_appends(out, "\nAvailable agent presets (names and descriptions only):\n");
    for (i = 0; i < count; ++i) {
        buf_appendf(out, "- %s: ", entries[i].name);
        jescape(out, entries[i].description ? entries[i].description : "");
        buf_appends(out, "\n");
    }
}
