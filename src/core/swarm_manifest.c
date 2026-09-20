#include "core/swarm_manifest.h"
#include "json/json.h"
#include "util/util.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static bool closed_object(yyjson_val *value, const char *const *fields, size_t count,
                          const char *const *required, size_t required_count) {
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) < required_count ||
        yyjson_obj_size(value) > count)
        return false;
    size_t i, max;
    yyjson_val *key, *child;
    yyjson_obj_foreach(value, i, max, key, child) {
        const char *name = yyjson_get_str(key);
        size_t len = yyjson_get_len(key);
        if (!name || strlen(name) != len || !field_allowed(name, fields, count) ||
            yyjson_obj_getn(value, name, len) != child)
            return false;
    }
    for (size_t field = 0; field < required_count; ++field)
        if (!yyjson_obj_get(value, required[field])) return false;
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

static void contract_free(tny_swarm_manifest_contract *contract) {
    if (!contract) return;
    free(contract->deliverable);
    for (size_t i = 0; i < contract->acceptance_count; ++i) free(contract->acceptance[i]);
    for (size_t i = 0; i < contract->dependency_count; ++i) free(contract->dependency_names[i]);
    free(contract->workspace_base);
    memset(contract, 0, sizeof *contract);
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

static int parse_contract(manifest_parser *parser, yyjson_val *value, const char *kind, bool root,
                          tny_swarm_manifest_contract *contract) {
    yyjson_val *deliverable = yyjson_obj_get(value, "deliverable");
    if (deliverable) {
        if (!bounded_text(deliverable, TNY_SWARM_MANIFEST_MAX_DELIVERABLE_BYTES, false))
            return fail(parser, "%s deliverable must be 1..%u non-blank UTF-8 bytes", kind,
                        TNY_SWARM_MANIFEST_MAX_DELIVERABLE_BYTES);
        contract->deliverable = copy_text(deliverable);
        if (!contract->deliverable) return fail(parser, "out of memory");
    }
    yyjson_val *acceptance = yyjson_obj_get(value, "acceptance");
    if (acceptance) {
        size_t count = yyjson_arr_size(acceptance);
        if (!yyjson_is_arr(acceptance) || !count || count > TNY_SWARM_MANIFEST_MAX_ACCEPTANCE)
            return fail(parser, "%s acceptance must contain 1..%u criteria", kind,
                        TNY_SWARM_MANIFEST_MAX_ACCEPTANCE);
        contract->acceptance_declared = true;
        size_t i, max;
        yyjson_val *criterion;
        yyjson_arr_foreach(acceptance, i, max, criterion) {
            if (!bounded_text(criterion, TNY_SWARM_MANIFEST_MAX_ACCEPTANCE_BYTES, false))
                return fail(parser, "%s acceptance criteria must be 1..%u non-blank UTF-8 bytes",
                            kind, TNY_SWARM_MANIFEST_MAX_ACCEPTANCE_BYTES);
            contract->acceptance[contract->acceptance_count] = copy_text(criterion);
            if (!contract->acceptance[contract->acceptance_count])
                return fail(parser, "out of memory");
            contract->acceptance_count++;
        }
    }
    yyjson_val *dependencies = yyjson_obj_get(value, "depends_on");
    yyjson_val *workspace = yyjson_obj_get(value, "workspace");
    if (root && (dependencies || workspace))
        return fail(parser, "root coordinator cannot declare depends_on or workspace");
    if (dependencies) {
        size_t count = yyjson_arr_size(dependencies);
        if (!yyjson_is_arr(dependencies) || count > TNY_SWARM_MANIFEST_MAX_DEPENDENCIES)
            return fail(parser, "%s depends_on may contain at most %u names", kind,
                        TNY_SWARM_MANIFEST_MAX_DEPENDENCIES);
        contract->dependencies_declared = true;
        size_t i, max;
        yyjson_val *dependency;
        yyjson_arr_foreach(dependencies, i, max, dependency) {
            if (!bounded_text(dependency, TNY_SWARM_MANIFEST_MAX_NAME_BYTES, true))
                return fail(parser,
                            "%s dependency names must be 1..%u UTF-8 bytes without surrounding "
                            "whitespace",
                            kind, TNY_SWARM_MANIFEST_MAX_NAME_BYTES);
            const char *name = yyjson_get_str(dependency);
            for (size_t j = 0; j < contract->dependency_count; ++j)
                if (strcmp(contract->dependency_names[j], name) == 0)
                    return fail(parser, "%s has duplicate dependency: %s", kind, name);
            contract->dependency_names[contract->dependency_count] = copy_text(dependency);
            if (!contract->dependency_names[contract->dependency_count])
                return fail(parser, "out of memory");
            contract->dependency_count++;
        }
    }
    if (workspace) {
        static const char *const policy_only[] = {"policy"};
        static const char *const with_base[] = {"policy", "base"};
        yyjson_val *base = yyjson_obj_get(workspace, "base");
        if (!(base ? exact_object(workspace, with_base, 2)
                   : exact_object(workspace, policy_only, 1)))
            return fail(parser, "%s workspace has missing, duplicate, or unknown fields", kind);
        yyjson_val *policy = yyjson_obj_get(workspace, "policy");
        if (!yyjson_is_str(policy) || strlen(yyjson_get_str(policy)) != yyjson_get_len(policy))
            return fail(parser,
                        "%s workspace policy must be shared_writable, shared_read_only or isolated",
                        kind);
        const char *policy_name = yyjson_get_str(policy);
        if (strcmp(policy_name, "shared_read_only") == 0 ||
            strcmp(policy_name, "shared_writable") == 0) {
            if (base) return fail(parser, "%s shared workspace cannot declare base", kind);
            contract->workspace_policy = strcmp(policy_name, "shared_read_only") == 0
                                             ? TNY_SWARM_WORKSPACE_SHARED_READ_ONLY
                                             : TNY_SWARM_WORKSPACE_SHARED_WRITABLE;
        } else if (strcmp(policy_name, "isolated") == 0) {
            contract->workspace_policy = TNY_SWARM_WORKSPACE_ISOLATED;
            if (base) {
                if (!bounded_text(base, TNY_SWARM_MANIFEST_MAX_WORKSPACE_BASE, false))
                    return fail(parser, "%s workspace base must be 1..%u non-blank UTF-8 bytes",
                                kind, TNY_SWARM_MANIFEST_MAX_WORKSPACE_BASE);
                contract->workspace_base = copy_text(base);
                if (!contract->workspace_base) return fail(parser, "out of memory");
            }
        } else {
            return fail(parser,
                        "%s workspace policy must be shared_writable, shared_read_only or isolated",
                        kind);
        }
        contract->workspace_declared = true;
    }
    return 0;
}

static int copy_actor(manifest_parser *parser, yyjson_val *value, const char *kind, bool root,
                      char **name_out, char **purpose_out, tny_swarm_manifest_contract *contract) {
    static const char *const fields[] = {"name",       "purpose",    "deliverable",
                                         "acceptance", "depends_on", "workspace"};
    static const char *const root_fields[] = {"name", "purpose", "deliverable", "acceptance"};
    static const char *const required[] = {"name", "purpose"};
    if (parser->manifest->version == TNY_SWARM_MANIFEST_VERSION_V1) {
        if (!exact_object(value, fields, 2))
            return fail(parser, "%s must contain exactly name and purpose", kind);
    } else {
        const char *const *allowed = root ? root_fields : fields;
        size_t allowed_count =
            root ? sizeof root_fields / sizeof *root_fields : sizeof fields / sizeof *fields;
        if (!closed_object(value, allowed, allowed_count, required,
                           sizeof required / sizeof *required)) {
            if (root && yyjson_is_obj(value) &&
                (yyjson_obj_get(value, "depends_on") || yyjson_obj_get(value, "workspace")))
                return fail(parser, "root coordinator cannot declare depends_on or workspace");
            return fail(parser, "%s has missing, duplicate, or unknown fields", kind);
        }
    }
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
    return parser->manifest->version == TNY_SWARM_MANIFEST_VERSION_V2
               ? parse_contract(parser, value, kind, root, contract)
               : 0;
}

static int contract_copy(tny_swarm_manifest_contract *out,
                         const tny_swarm_manifest_contract *source) {
    out->workspace_policy = source->workspace_policy;
    out->acceptance_declared = source->acceptance_declared;
    out->dependencies_declared = source->dependencies_declared;
    out->workspace_declared = source->workspace_declared;
    if (source->deliverable && !(out->deliverable = xstrdup(source->deliverable))) return -1;
    for (size_t i = 0; i < source->acceptance_count; ++i) {
        out->acceptance[i] = xstrdup(source->acceptance[i]);
        if (!out->acceptance[i]) return -1;
        out->acceptance_count++;
    }
    for (size_t i = 0; i < source->dependency_count; ++i) {
        out->dependency_names[i] = xstrdup(source->dependency_names[i]);
        if (!out->dependency_names[i]) return -1;
        out->dependencies[i] = source->dependencies[i];
        out->dependency_count++;
    }
    if (source->workspace_base && !(out->workspace_base = xstrdup(source->workspace_base)))
        return -1;
    return 0;
}

static void append_actor(buf_t *out, const char *name, const char *purpose,
                         const tny_swarm_manifest_contract *contract, unsigned version) {
    buf_appends(out, "{\"name\":");
    jescape(out, name);
    buf_appends(out, ",\"purpose\":");
    jescape(out, purpose);
    if (version == TNY_SWARM_MANIFEST_VERSION_V2) {
        if (contract->deliverable) {
            buf_appends(out, ",\"deliverable\":");
            jescape(out, contract->deliverable);
        }
        if (contract->acceptance_declared) {
            buf_appends(out, ",\"acceptance\":[");
            for (size_t i = 0; i < contract->acceptance_count; ++i) {
                if (i) buf_appends(out, ",");
                jescape(out, contract->acceptance[i]);
            }
            buf_appends(out, "]");
        }
        if (contract->dependencies_declared) {
            buf_appends(out, ",\"depends_on\":[");
            for (size_t i = 0; i < contract->dependency_count; ++i) {
                if (i) buf_appends(out, ",");
                jescape(out, contract->dependency_names[i]);
            }
            buf_appends(out, "]");
        }
        if (contract->workspace_declared) {
            buf_appends(out, ",\"workspace\":{\"policy\":");
            jescape(out, tny_swarm_manifest_workspace_name(contract->workspace_policy));
            if (contract->workspace_base) {
                buf_appends(out, ",\"base\":");
                jescape(out, contract->workspace_base);
            }
            buf_appends(out, "}");
        }
    }
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
        uint64_t parsed = yyjson_get_uint(version);
        if (!yyjson_is_uint(version) ||
            (parsed != TNY_SWARM_MANIFEST_VERSION_V1 && parsed != TNY_SWARM_MANIFEST_VERSION_V2))
            return fail(parser, "swarm version must be 1 or 2");
        parser->manifest->version = (unsigned)parsed;
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
    if (copy_actor(parser, coordinator, root ? "root coordinator" : "coordinator", root,
                   &group->coordinator_name, &group->coordinator_purpose,
                   &group->coordinator_contract) != 0)
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
        if (!participant->name || !participant->purpose ||
            contract_copy(&participant->contract, &group->coordinator_contract) != 0)
            return fail(parser, "out of memory");
        group->coordinator_participant = participant_index;
    }

    if (root)
        buf_appendf(&parser->canonical, "{\"version\":%u,\"purpose\":", parser->manifest->version);
    else buf_appends(&parser->canonical, "{\"purpose\":");
    jescape(&parser->canonical, group->purpose);
    buf_appends(&parser->canonical, ",\"coordinator\":");
    append_actor(&parser->canonical, group->coordinator_name, group->coordinator_purpose,
                 &group->coordinator_contract, parser->manifest->version);
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
        if (copy_actor(parser, agent, "agent", false, &participant->name, &participant->purpose,
                       &participant->contract) != 0)
            return -1;
        participant->group = group_index;
        participant->coordinator = false;
        if (i) buf_appends(&parser->canonical, ",");
        append_actor(&parser->canonical, participant->name, participant->purpose,
                     &participant->contract, parser->manifest->version);
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

static size_t participant_named(const tny_swarm_manifest *manifest, const char *name) {
    for (size_t i = 0; i < manifest->participant_count; ++i)
        if (strcmp(manifest->participants[i].name, name) == 0) return i;
    return SIZE_MAX;
}

static int resolve_dependencies(manifest_parser *parser) {
    tny_swarm_manifest *manifest = parser->manifest;
    for (size_t i = 0; i < manifest->participant_count; ++i) {
        tny_swarm_manifest_participant *participant = &manifest->participants[i];
        for (size_t d = 0; d < participant->contract.dependency_count; ++d) {
            const char *name = participant->contract.dependency_names[d];
            size_t dependency = participant_named(manifest, name);
            if (dependency == SIZE_MAX) {
                if (strcmp(name, manifest->groups[0].coordinator_name) == 0)
                    return fail(parser, "%s cannot depend on root coordinator %s",
                                participant->name, name);
                return fail(parser, "%s has unknown dependency: %s", participant->name, name);
            }
            if (dependency == i)
                return fail(parser, "%s cannot depend on itself", participant->name);
            participant->contract.dependencies[d] = dependency;
            if (participant->coordinator)
                manifest->groups[participant->group].coordinator_contract.dependencies[d] =
                    dependency;
        }
    }

    bool done[TNY_SWARM_MANIFEST_MAX_PARTICIPANTS] = {false};
    size_t completed = 0;
    while (completed < manifest->participant_count) {
        bool progress = false;
        for (size_t i = 0; i < manifest->participant_count; ++i) {
            if (done[i]) continue;
            bool ready = true;
            const tny_swarm_manifest_contract *contract = &manifest->participants[i].contract;
            for (size_t d = 0; d < contract->dependency_count; ++d)
                if (!done[contract->dependencies[d]]) ready = false;
            if (ready) {
                done[i] = true;
                completed++;
                progress = true;
            }
        }
        if (!progress) return fail(parser, "swarm dependencies contain a cycle");
    }
    return 0;
}

void tny_swarm_manifest_free(tny_swarm_manifest *manifest) {
    if (!manifest) return;
    for (size_t i = 0; i < manifest->group_count; ++i) {
        free(manifest->groups[i].purpose);
        free(manifest->groups[i].coordinator_name);
        free(manifest->groups[i].coordinator_purpose);
        contract_free(&manifest->groups[i].coordinator_contract);
    }
    for (size_t i = 0; i < manifest->participant_count; ++i) {
        free(manifest->participants[i].name);
        free(manifest->participants[i].purpose);
        contract_free(&manifest->participants[i].contract);
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
    if (!rc) rc = resolve_dependencies(&parser);
    if (!rc) {
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
    /* Validate the descriptor without waiting for a FIFO writer. File-based
     * configuration is bounded input, not an unbounded streaming transport. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (err && errlen) snprintf(err, errlen, "could not read swarm file");
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        if (err && errlen) snprintf(err, errlen, "swarm file must be a readable regular file");
        return -1;
    }
    if (st.st_size <= 0 || (uintmax_t)st.st_size > TNY_SWARM_MANIFEST_MAX_BYTES) {
        close(fd);
        if (err && errlen)
            snprintf(err, errlen, "swarm file must contain 1..%u bytes",
                     TNY_SWARM_MANIFEST_MAX_BYTES);
        return -1;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        if (err && errlen) snprintf(err, errlen, "could not read swarm file");
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

const char *tny_swarm_manifest_workspace_name(tny_swarm_manifest_workspace_policy policy) {
    switch (policy) {
    case TNY_SWARM_WORKSPACE_SHARED_WRITABLE: return "shared_writable";
    case TNY_SWARM_WORKSPACE_SHARED_READ_ONLY: return "shared_read_only";
    case TNY_SWARM_WORKSPACE_ISOLATED: return "isolated";
    }
    return NULL;
}
