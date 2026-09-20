#include "core/swarm_manifest.h"
#include "json/json.h"
#include "util/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    tny_swarm_manifest *manifest;
    size_t capacity;
    buf_t canonical;
    char *err;
    size_t errlen;
} manifest_parser;

static int fail(manifest_parser *parser, const char *fmt, ...) {
    if (parser->err && parser->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(parser->err, parser->errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static bool field_allowed(const char *name, const char *const *fields, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (strcmp(name, fields[i]) == 0) return true;
    return false;
}

/* yyjson preserves duplicate members. Requiring the expected member count and
 * requiring each iteration value to be the object's lookup result rejects
 * duplicates, including differently escaped spellings of the same key. */
static bool exact_object(yyjson_val *value, const char *const *fields, size_t count) {
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) != count) return false;
    size_t i, max;
    yyjson_val *key, *child;
    yyjson_obj_foreach(value, i, max, key, child) {
        const char *name = yyjson_get_str(key);
        size_t len = yyjson_get_len(key);
        if (!name || strlen(name) != len || !field_allowed(name, fields, count) ||
            yyjson_obj_getn(value, name, len) != child)
            return false;
    }
    for (size_t field = 0; field < count; ++field)
        if (!yyjson_obj_get(value, fields[field])) return false;
    return true;
}

static bool ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool bounded_text(yyjson_val *value, size_t max, bool name) {
    if (!yyjson_is_str(value)) return false;
    const char *text = yyjson_get_str(value);
    size_t len = yyjson_get_len(value);
    if (!text || !len || len > max || strlen(text) != len || !utf8_valid_bytes(text, len))
        return false;
    bool content = false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!ascii_space(c)) content = true;
        if (name && (c < 0x20 || c == 0x7f)) return false;
    }
    if (name && (ascii_space((unsigned char)text[0]) || ascii_space((unsigned char)text[len - 1])))
        return false;
    return content;
}

static char *copy_text(yyjson_val *value) {
    return xstrndup(yyjson_get_str(value), yyjson_get_len(value));
}

static bool name_used(const tny_swarm_manifest *manifest, const char *name) {
    for (size_t i = 0; i < manifest->group_count; ++i)
        if (manifest->groups[i].coordinator_name &&
            strcmp(manifest->groups[i].coordinator_name, name) == 0)
            return true;
    for (size_t i = 0; i < manifest->participant_count; ++i)
        if (manifest->participants[i].name && strcmp(manifest->participants[i].name, name) == 0)
            return true;
    return false;
}

static int copy_named_purpose(manifest_parser *parser, yyjson_val *value, const char *kind,
                              char **name_out, char **purpose_out) {
    static const char *const fields[] = {"name", "purpose"};
    if (!exact_object(value, fields, sizeof fields / sizeof *fields))
        return fail(parser, "%s must contain exactly name and purpose", kind);
    yyjson_val *name = yyjson_obj_get(value, "name");
    yyjson_val *purpose = yyjson_obj_get(value, "purpose");
    if (!bounded_text(name, TNY_SWARM_MANIFEST_MAX_NAME_BYTES, true))
        return fail(parser, "%s name must be 1..%u UTF-8 bytes without surrounding whitespace",
                    kind, TNY_SWARM_MANIFEST_MAX_NAME_BYTES);
    if (!bounded_text(purpose, TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES, false))
        return fail(parser, "%s purpose must be 1..%u non-blank UTF-8 bytes", kind,
                    TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES);
    const char *name_text = yyjson_get_str(name);
    if (name_used(parser->manifest, name_text))
        return fail(parser, "duplicate swarm participant name: %s", name_text);
    *name_out = copy_text(name);
    *purpose_out = copy_text(purpose);
    if (!*name_out || !*purpose_out) return fail(parser, "out of memory");
    return 0;
}

static void append_named_purpose(buf_t *out, const char *name, const char *purpose) {
    buf_appends(out, "{\"name\":");
    jescape(out, name);
    buf_appends(out, ",\"purpose\":");
    jescape(out, purpose);
    buf_appends(out, "}");
}

static int parse_group(manifest_parser *parser, yyjson_val *value, unsigned depth, size_t parent,
                       bool root) {
    static const char *const root_fields[] = {"version", "purpose", "coordinator", "agents",
                                              "swarms"};
    static const char *const group_fields[] = {"purpose", "coordinator", "agents", "swarms"};
    const char *const *fields = root ? root_fields : group_fields;
    size_t field_count = root ? sizeof root_fields / sizeof *root_fields
                              : sizeof group_fields / sizeof *group_fields;
    if (!exact_object(value, fields, field_count))
        return fail(parser, "swarm group has missing, duplicate, or unknown fields");
    if (depth > TNY_SWARM_MANIFEST_MAX_DEPTH)
        return fail(parser, "swarm nesting exceeds maximum depth %u", TNY_SWARM_MANIFEST_MAX_DEPTH);
    if (parser->manifest->group_count >= TNY_SWARM_MANIFEST_MAX_GROUPS)
        return fail(parser, "swarm has too many nested groups");
    if (root) {
        yyjson_val *version = yyjson_obj_get(value, "version");
        if (!yyjson_is_uint(version) || yyjson_get_uint(version) != TNY_SWARM_MANIFEST_VERSION)
            return fail(parser, "swarm version must be %u", TNY_SWARM_MANIFEST_VERSION);
    }
    yyjson_val *purpose = yyjson_obj_get(value, "purpose");
    yyjson_val *coordinator = yyjson_obj_get(value, "coordinator");
    yyjson_val *agents = yyjson_obj_get(value, "agents");
    yyjson_val *swarms = yyjson_obj_get(value, "swarms");
    if (!bounded_text(purpose, TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES, false))
        return fail(parser, "swarm purpose must be 1..%u non-blank UTF-8 bytes",
                    TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES);
    if (!yyjson_is_arr(agents) || !yyjson_is_arr(swarms))
        return fail(parser, "every swarm group requires agents and swarms arrays");
    if (yyjson_arr_size(agents) > TNY_SWARM_MANIFEST_MAX_PARTICIPANTS ||
        yyjson_arr_size(swarms) > TNY_SWARM_MANIFEST_MAX_PARTICIPANTS)
        return fail(parser, "swarm group arrays may contain at most %u entries",
                    TNY_SWARM_MANIFEST_MAX_PARTICIPANTS);

    size_t group_index = parser->manifest->group_count++;
    tny_swarm_manifest_group *group = &parser->manifest->groups[group_index];
    group->parent = parent;
    group->depth = depth;
    group->coordinator_participant = SIZE_MAX;
    group->purpose = copy_text(purpose);
    if (!group->purpose) return fail(parser, "out of memory");
    if (copy_named_purpose(parser, coordinator, "coordinator", &group->coordinator_name,
                           &group->coordinator_purpose) != 0)
        return -1;

    if (!root) {
        if (parser->manifest->participant_count >= parser->capacity)
            return fail(parser, "swarm defines more than the available capacity of %zu",
                        parser->capacity);
        size_t participant_index = parser->manifest->participant_count++;
        tny_swarm_manifest_participant *participant =
            &parser->manifest->participants[participant_index];
        participant->name = xstrdup(group->coordinator_name);
        participant->purpose = xstrdup(group->coordinator_purpose);
        participant->group = group_index;
        participant->coordinator = true;
        if (!participant->name || !participant->purpose) return fail(parser, "out of memory");
        group->coordinator_participant = participant_index;
    }

    if (root) buf_appends(&parser->canonical, "{\"version\":1,\"purpose\":");
    else buf_appends(&parser->canonical, "{\"purpose\":");
    jescape(&parser->canonical, group->purpose);
    buf_appends(&parser->canonical, ",\"coordinator\":");
    append_named_purpose(&parser->canonical, group->coordinator_name, group->coordinator_purpose);
    buf_appends(&parser->canonical, ",\"agents\":[");

    size_t i, max;
    yyjson_val *agent;
    yyjson_arr_foreach(agents, i, max, agent) {
        if (parser->manifest->participant_count >= parser->capacity)
            return fail(parser, "swarm defines more than the available capacity of %zu",
                        parser->capacity);
        size_t participant_index = parser->manifest->participant_count++;
        tny_swarm_manifest_participant *participant =
            &parser->manifest->participants[participant_index];
        if (copy_named_purpose(parser, agent, "agent", &participant->name, &participant->purpose) !=
            0)
            return -1;
        participant->group = group_index;
        participant->coordinator = false;
        if (i) buf_appends(&parser->canonical, ",");
        append_named_purpose(&parser->canonical, participant->name, participant->purpose);
    }
    buf_appends(&parser->canonical, "],\"swarms\":[");
    yyjson_val *nested;
    yyjson_arr_foreach(swarms, i, max, nested) {
        if (i) buf_appends(&parser->canonical, ",");
        if (parse_group(parser, nested, depth + 1, group_index, false) != 0) return -1;
    }
    buf_appends(&parser->canonical, "]}");
    return parser->canonical.oom ? fail(parser, "out of memory") : 0;
}

void tny_swarm_manifest_free(tny_swarm_manifest *manifest) {
    if (!manifest) return;
    for (size_t i = 0; i < manifest->group_count; ++i) {
        free(manifest->groups[i].purpose);
        free(manifest->groups[i].coordinator_name);
        free(manifest->groups[i].coordinator_purpose);
    }
    for (size_t i = 0; i < manifest->participant_count; ++i) {
        free(manifest->participants[i].name);
        free(manifest->participants[i].purpose);
    }
    free(manifest->canonical_json);
    free(manifest);
}

int tny_swarm_manifest_parse(const char *bytes, size_t len, size_t capacity,
                             tny_swarm_manifest **out, char *err, size_t errlen) {
    if (out) *out = NULL;
    if (err && errlen) err[0] = 0;
    if (!out) {
        if (err && errlen) snprintf(err, errlen, "manifest output is required");
        return -1;
    }
    if (!bytes || !len || len > TNY_SWARM_MANIFEST_MAX_BYTES) {
        if (err && errlen)
            snprintf(err, errlen, "swarm file must contain 1..%u bytes",
                     TNY_SWARM_MANIFEST_MAX_BYTES);
        return -1;
    }
    if (!capacity || capacity > TNY_SWARM_MANIFEST_MAX_PARTICIPANTS) {
        if (err && errlen)
            snprintf(err, errlen, "swarm capacity must be between 1 and %u",
                     TNY_SWARM_MANIFEST_MAX_PARTICIPANTS);
        return -1;
    }
    yyjson_read_err read_error = {0};
    yyjson_doc *doc = yyjson_read_opts((char *)(uintptr_t)bytes, len, 0, jallocator(), &read_error);
    if (!doc) {
        if (err && errlen) snprintf(err, errlen, "invalid swarm JSON at byte %zu", read_error.pos);
        return -1;
    }
    tny_swarm_manifest *manifest = calloc(1, sizeof *manifest);
    manifest_parser parser = {
        .manifest = manifest, .capacity = capacity, .err = err, .errlen = errlen};
    buf_init(&parser.canonical);
    int rc = manifest ? parse_group(&parser, yyjson_doc_get_root(doc), 1, SIZE_MAX, true) : -1;
    if (!manifest && err && errlen) snprintf(err, errlen, "out of memory");
    if (!rc && !manifest->participant_count)
        rc = fail(&parser, "swarm must define at least one launched participant");
    if (!rc) {
        manifest->version = TNY_SWARM_MANIFEST_VERSION;
        manifest->canonical_len = parser.canonical.len;
        manifest->canonical_json = buf_detach(&parser.canonical);
        if (!manifest->canonical_json) rc = fail(&parser, "out of memory");
    }
    yyjson_doc_free(doc);
    buf_free(&parser.canonical);
    if (rc) {
        tny_swarm_manifest_free(manifest);
        return -1;
    }
    *out = manifest;
    return 0;
}

int tny_swarm_manifest_parse_file(const char *path, size_t capacity, tny_swarm_manifest **out,
                                  char *err, size_t errlen) {
    if (out) *out = NULL;
    if (err && errlen) err[0] = 0;
    if (!path || !*path) {
        if (err && errlen) snprintf(err, errlen, "swarm file must be a readable regular file");
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (err && errlen) snprintf(err, errlen, "could not read swarm file");
        return -1;
    }
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(file);
        if (err && errlen) snprintf(err, errlen, "swarm file must be a readable regular file");
        return -1;
    }
    if (st.st_size <= 0 || (uintmax_t)st.st_size > TNY_SWARM_MANIFEST_MAX_BYTES) {
        fclose(file);
        if (err && errlen)
            snprintf(err, errlen, "swarm file must contain 1..%u bytes",
                     TNY_SWARM_MANIFEST_MAX_BYTES);
        return -1;
    }
    char *bytes = malloc(TNY_SWARM_MANIFEST_MAX_BYTES + 1u);
    size_t len = bytes ? fread(bytes, 1, TNY_SWARM_MANIFEST_MAX_BYTES + 1u, file) : 0;
    bool read_failed = !bytes || ferror(file);
    fclose(file);
    if (read_failed || !len || len > TNY_SWARM_MANIFEST_MAX_BYTES) {
        free(bytes);
        if (err && errlen)
            snprintf(err, errlen, "%s",
                     read_failed ? "could not read swarm file" : "swarm file exceeds 65536 bytes");
        return -1;
    }
    int rc = tny_swarm_manifest_parse(bytes, len, capacity, out, err, errlen);
    free(bytes);
    return rc;
}
