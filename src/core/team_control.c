#include "core/team_control.h"
#include "core/team_runtime.h"
#include "core/session.h"
#include "util/image_io.h"
#include "util/jobs_host.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void nullable_string(buf_t *out, const char *value) {
    if (value) jescape(out, value);
    else buf_appends(out, "null");
}

static bool same(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
static int fail(char *err, size_t n, const char *text) {
    if (err && n) snprintf(err, n, "%s", text);
    return 1;
}
static bool bounded_string(yyjson_val *v, size_t max) {
    const char *s = yyjson_get_str(v);
    return s && yyjson_get_len(v) > 0 && yyjson_get_len(v) <= max && strlen(s) == yyjson_get_len(v);
}
static bool uint_field(yyjson_val *obj, const char *key, uint64_t min, uint64_t max,
                       bool required) {
    yyjson_val *v = jget(obj, key);
    if (!v) return !required;
    return yyjson_is_uint(v) && yyjson_get_uint(v) >= min && yyjson_get_uint(v) <= max;
}
static bool session_id_valid(const char *id) {
    if (!id || strlen(id) != 16) return false;
    return strspn(id, "0123456789abcdef") == 16;
}
static bool terminal(const char *state) {
    return same(state, "succeeded") || same(state, "failed") || same(state, "cancelled") ||
           same(state, "interrupted");
}

tny_team_op tny_team_op_parse(const char *name) {
    static const char *const names[] = {"start",    "status", "collect",
                                        "wait-any", "cancel", "verify"};
    for (int i = 0; i < TNY_TEAM_NONE; ++i)
        if (same(name, names[i])) return (tny_team_op)i;
    return TNY_TEAM_NONE;
}
const char *tny_team_permission_tool(tny_team_op op) {
    static const char *const names[] = {"team_start",    "team_status", "team_collect",
                                        "team_wait_any", "team_cancel", "team_verify"};
    return op >= TNY_TEAM_START && op < TNY_TEAM_NONE ? names[op] : NULL;
}
bool tny_team_op_is_sensitive(tny_team_op op) {
    return op == TNY_TEAM_START || op == TNY_TEAM_CANCEL || op == TNY_TEAM_VERIFY;
}

/* Duplicate JSON keys are ambiguous permission identities. Reject recursively,
 * including nested job options, before delegating to the jobs validator. */
static bool unique_keys(yyjson_val *value, unsigned depth) {
    if (depth > 32) return false;
    size_t i, max;
    yyjson_val *key, *child;
    if (yyjson_is_obj(value)) {
        yyjson_obj_foreach(value, i, max, key, child) {
            const char *name = yyjson_get_str(key);
            if (!name || strlen(name) != yyjson_get_len(key)) return false;
            if (yyjson_obj_getn(value, name, yyjson_get_len(key)) != child ||
                !unique_keys(child, depth + 1))
                return false;
        }
    } else if (yyjson_is_arr(value)) {
        yyjson_arr_foreach(value, i, max, child) if (!unique_keys(child, depth + 1)) return false;
    } else if (yyjson_is_str(value) && strlen(yyjson_get_str(value)) != yyjson_get_len(value)) {
        return false;
    }
    return true;
}

static bool team_shape(yyjson_val *root, bool request) {
    yyjson_val *items = jget(root, "items");
    if (!jget_bool(root, "dag", false) ||
        !same(jget_str(root, request ? "kind" : "job_kind"), "ask") || !yyjson_is_arr(items) ||
        yyjson_arr_size(items) < 1 || yyjson_arr_size(items) > TNY_JOBS_MAX_ITEMS)
        return false;
    int leads = 0, workers = 0;
    size_t i, max;
    yyjson_val *item;
    yyjson_arr_foreach(items, i, max, item) {
        if (same(jget_str(item, "role"), "lead")) ++leads;
        else if (same(jget_str(item, "role"), "worker")) ++workers;
        else return false;
    }
    const char *label = jget_str(jget(root, "admission"), "label");
    bool single_swarm_worker =
        workers == 1 && leads == 0 && (request || (label && str_starts(label, "swarm_")));
    return (workers >= 2 || single_swarm_worker) &&
           (leads == 1 ||
            (leads == 0 && (request || session_id_valid(jget_str(root, "parent_session_id")))));
}

static int validate(tny_team_op op, yyjson_val *args, char *err, size_t n) {
    if (op < TNY_TEAM_START || op >= TNY_TEAM_NONE || !yyjson_is_obj(args) || !unique_keys(args, 0))
        return fail(err, n, "expected an unambiguous team request object");
    if (op == TNY_TEAM_START) {
        if (!team_shape(args, true))
            return fail(err, n,
                        "start requires kind:ask, dag:true, one explicit lead and at least two "
                        "explicit workers");
        /* Lineage must come from the captured runtime caller. */
        if (jget(args, "id") || jget(args, "run_id") || jget(args, "parent_session_id") ||
            jget(args, "session_id"))
            return fail(err, n, "caller identity cannot be supplied in a request");
        return 0; /* Remaining execution settings belong to jobs_detail/run. */
    }
    size_t i, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(args, i, max, key, value) {
        (void)value;
        const char *s = yyjson_get_str(key);
        bool allowed = same(s, "id") || same(s, "expected_attempt") || same(s, "item") ||
                       (op == TNY_TEAM_COLLECT && same(s, "max_bytes")) ||
                       (op == TNY_TEAM_WAIT_ANY && (same(s, "seen") || same(s, "timeout_ms"))) ||
                       (op == TNY_TEAM_VERIFY &&
                        (same(s, "command") || same(s, "cwd") || same(s, "timeout_ms")));
        if (!allowed) return fail(err, n, "unknown field in team request");
    }
    if (!tny_jobs_valid_id(jget_str(args, "id")) || yyjson_get_len(jget(args, "id")) != 32 ||
        !uint_field(args, "item", 0, TNY_JOBS_MAX_ITEMS - 1,
                    op == TNY_TEAM_COLLECT || op == TNY_TEAM_VERIFY) ||
        !uint_field(args, "expected_attempt", 1, INT_MAX,
                    op == TNY_TEAM_CANCEL || op == TNY_TEAM_VERIFY) ||
        !uint_field(args, "max_bytes", 1, TNY_JOBS_LOG_READ_MAX, false) ||
        !uint_field(args, "timeout_ms", 0, TNY_TEAM_WAIT_MAX_MS, false))
        return fail(err, n, "invalid run, item, attempt or byte/time bound");
    if (op == TNY_TEAM_VERIFY && (!bounded_string(jget(args, "command"), TNY_JOBS_PROMPT_MAX) ||
                                  !bounded_string(jget(args, "cwd"), TNY_IMAGE_IO_PATH_MAX) ||
                                  jget_str(args, "cwd")[0] != '/' ||
                                  !uint_field(args, "timeout_ms", 1, TNY_TEAM_WAIT_MAX_MS, true)))
        return fail(err, n,
                    "verify requires an explicit command, absolute cwd and bounded timeout_ms");
    yyjson_val *seen = jget(args, "seen");
    if (seen && (!yyjson_is_arr(seen) || yyjson_arr_size(seen) > TNY_JOBS_MAX_ITEMS))
        return fail(err, n, "seen must be a bounded list of item/attempt pairs");
    yyjson_arr_foreach(seen, i, max, value) {
        if (!yyjson_is_obj(value) || yyjson_obj_size(value) != 2 ||
            !uint_field(value, "item", 0, TNY_JOBS_MAX_ITEMS - 1, true) ||
            !uint_field(value, "attempt", 1, INT_MAX, true))
            return fail(err, n, "invalid seen item/attempt pair");
    }
    return 0;
}

tny_team_op tny_team_parse_argv(int argc, char **argv, const char *stdin_text, size_t stdin_len,
                                char **request_out, const char **error) {
    *request_out = NULL;
    *error = "usage: team OP --request FILE|- [--json]";
    if (argc < 3) return TNY_TEAM_NONE;
    tny_team_op op = tny_team_op_parse(argv[0]);
    const char *path = NULL;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "--request") && !path && i + 1 < argc) path = argv[++i];
        else if (same(argv[i], "--json") && !json) json = true;
        else return TNY_TEAM_NONE;
    }
    if (!path || op == TNY_TEAM_NONE) return TNY_TEAM_NONE;
    buf_t data = {0};
    if (same(path, "-")) {
        if (!stdin_text || !stdin_len || stdin_len > TNY_JOBS_REQUEST_MAX) return TNY_TEAM_NONE;
        buf_append(&data, stdin_text, stdin_len);
    } else if (tny_image_io_read_bounded(path, TNY_JOBS_REQUEST_MAX, &data)) {
        *error = "cannot read bounded request file";
        return TNY_TEAM_NONE;
    }
    yyjson_doc *doc = !buf_oom(&data) ? jparse(data.data, data.len) : NULL;
    char err[256] = "";
    bool valid = doc && validate(op, yyjson_doc_get_root(doc), err, sizeof err) == 0;
    if (valid) *request_out = jwrite_val(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    buf_free(&data);
    if (!*request_out) {
        *error = "invalid team request document";
        return TNY_TEAM_NONE;
    }
    *error = NULL;
    return op;
}

/* Canonicalize the trusted state-root prefix only (e.g. macOS /var). Never
 * resolve a record-supplied leaf; confined reads must still reject its links. */
static char *state_path(const tny_ctx *ctx, const char *relative) {
    char *base = path_abs(ctx->tny_dir);
    char *path = base ? path_join(base, relative) : NULL;
    free(base);
    return path;
}
static char *recorded_state_path(const tny_ctx *ctx, const char *path) {
    if (!path) return NULL;
    size_t prefix = strlen(ctx->tny_dir);
    if (strncmp(path, ctx->tny_dir, prefix) == 0 && path[prefix] == '/')
        return state_path(ctx, path + prefix + 1);
    char *base = path_abs(ctx->tny_dir);
    char *result = base && path_is_within(base, path) ? xstrdup(path) : NULL;
    free(base);
    return result;
}
static char *run_dir(const tny_ctx *ctx, const char *id) {
    char *root = state_path(ctx, "jobs");
    char *dir = root ? path_join(root, id) : NULL;
    free(root);
    return dir;
}
static yyjson_doc *read_record(const tny_ctx *ctx, const char *id, char *err, size_t n) {
    char *dir = run_dir(ctx, id);
    char *path = dir ? path_join(dir, "job.json") : NULL;
    buf_t raw = {0};
    char *base = path_abs(ctx->tny_dir);
    int rc = path && base ? tny_image_io_read_confined(base, path, TNY_JOBS_PAYLOAD_MAX, &raw) : -1;
    free(base);
    yyjson_doc *doc = rc == 0 ? jparse(raw.data, raw.len) : NULL;
    buf_free(&raw);
    free(path);
    free(dir);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || jget_int(root, "version", 0) != TNY_JOBS_SCHEMA_VERSION || !unique_keys(root, 0) ||
        !same(jget_str(root, "id"), id) || !same(jget_str(root, "run_id"), id) ||
        !uint_field(root, "attempt", 1, INT_MAX, true) || !team_shape(root, false)) {
        yyjson_doc_free(doc);
        fail(err, n, "no valid team record at this run identity");
        return NULL;
    }
    return doc;
}

/* Returns member index, -1 for operator/parent, -2 on refusal. Roles are
 * descriptive; a role label never becomes the submitting parent's authority. */
static int authority(const tny_team_caller *caller, yyjson_val *root, char *err, size_t n) {
    if (caller->local_operator) return -1;
    if (same(caller->session_id, jget_str(root, "parent_session_id"))) return -1;
    yyjson_val *item = caller->task_index >= 0
                           ? yyjson_arr_get(jget(root, "items"), (size_t)caller->task_index)
                           : NULL;
    if (!same(caller->run_id, jget_str(root, "id")) || caller->attempt < 1 ||
        jget_int(root, "attempt", 0) != caller->attempt ||
        jget_int(item, "attempt", 0) != caller->attempt ||
        (!tny_team_capability_matches(root, caller->task_index, caller->attempt,
                                      caller->capability) &&
         !same(caller->session_id, jget_str(item, "session_id")))) {
        fail(err, n, "caller is not the parent or a current fenced member of this run");
        return -2;
    }
    return caller->task_index;
}

static int preflight(tny_ctx *ctx, const tny_team_caller *caller, tny_team_op op, yyjson_val *args,
                     yyjson_doc **record, char *err, size_t n) {
    *record = NULL;
    if (!ctx || !caller || !ctx->tny_dir || !ctx->cwd || ctx->ssh_host || ctx->library_mode ||
        ctx->no_save || !tny_jobs_execution_supported())
        return fail(err, n,
                    "team control requires a native, saved, local CLI runtime; SSH/embedded/wasm "
                    "are unsupported");
    if (!caller->local_operator && !session_id_valid(caller->session_id))
        return fail(err, n, "a captured runtime session identity is required");
    if (validate(op, args, err, n)) return 1;
    if (op == TNY_TEAM_START) {
        if (yyjson_arr_size(jget(args, "items")) == 1 && !ctx->swarm_cap)
            return fail(err, n, "one-worker team start requires a captured swarm lead");
        if (caller->local_operator) {
            size_t i, count;
            yyjson_val *item;
            bool lead = false;
            yyjson_arr_foreach(jget(args, "items"), i, count,
                               item) if (same(jget_str(item, "role"), "lead")) lead = true;
            if (!lead)
                return fail(err, n,
                            "operator start needs a lead item; worker-only start "
                            "requires a captured native parent session");
        }
        return 0;
    }
    *record = read_record(ctx, jget_str(args, "id"), err, n);
    if (!*record) return 1;
    yyjson_val *root = yyjson_doc_get_root(*record);
    int member = authority(caller, root, err, n);
    if (member == -2) return 1;
    if (jget(args, "item") &&
        !yyjson_arr_get(jget(root, "items"), (size_t)jget_int(args, "item", -1)))
        return fail(err, n, "no such task in this run");
    if (member >= 0 && op != TNY_TEAM_STATUS && jget_int(args, "item", -1) != member)
        return fail(err, n, "members may collect, wait for or cancel only their own task");
    if (jget(args, "expected_attempt") &&
        jget_int(args, "expected_attempt", 0) != jget_int(root, "attempt", -1))
        return fail(err, n, "stale expected_attempt");
    return 0;
}

char *tny_team_detail(tny_ctx *ctx, const tny_team_caller *caller, tny_team_op op, yyjson_val *args,
                      char **error) {
    *error = NULL;
    char err[256] = "";
    yyjson_doc *record = NULL;
    int rc = preflight(ctx, caller, op, args, &record, err, sizeof err);
    yyjson_doc_free(record);
    if (rc) {
        *error = xstrdup(err);
        return NULL;
    }
    char *detail = op == TNY_TEAM_START ? tny_jobs_detail(ctx, TNY_JOBS_OP_SUBMIT, args, error)
                                        : jwrite_val(args);
    if (!detail) return NULL;
    buf_t out = {0};
    buf_appends(&out, "{\"operation\":");
    nullable_string(&out, tny_team_permission_tool(op));
    buf_appendf(&out, ",\"local_operator\":%s,\"caller_session\":",
                caller->local_operator ? "true" : "false");
    nullable_string(&out, caller->session_id);
    buf_appends(&out, ",\"caller_run\":");
    nullable_string(&out, caller->run_id);
    buf_appendf(&out,
                ",\"caller_task\":%d,\"caller_attempt\":%d,\"request_detail\":", caller->task_index,
                caller->attempt);
    nullable_string(&out, detail);
    buf_appends(&out, "}");
    free(detail);
    if (buf_oom(&out)) {
        buf_free(&out);
        *error = xstrdup("permission detail allocation failed");
        return NULL;
    }
    return buf_detach(&out);
}

static void envelope(buf_t *out, const char *kind, const char *id) {
    buf_appends(out, "{\"kind\":");
    nullable_string(out, kind);
    buf_appends(out, ",\"schema_version\":1,\"run_id\":");
    nullable_string(out, id);
    buf_appends(out, ",\"verification\":\"unverified\",\"integration\":\"not_recorded\"");
}
static int finish(buf_t *out, char *err, size_t n, int rc) {
    if (buf_oom(out)) {
        buf_clear(out);
        return fail(err, n, "team result allocation failed");
    }
    return rc;
}

static bool already_seen(yyjson_val *args, int index, int64_t attempt) {
    size_t i, max;
    yyjson_val *entry;
    yyjson_arr_foreach(jget(args, "seen"), i, max,
                       entry) if (jget_int(entry, "item", -1) == index &&
                                  jget_int(entry, "attempt", -1) == attempt) return true;
    return false;
}

/* A status read can create a missing owner/state lock or project an abandoned
 * owner, both of which notify a directory watch. Drain those self-generated
 * hints, then confirm that the private record still describes the public
 * snapshot. A real update racing the drain is either visible here or remains
 * queued for watch_next(), so it cannot be lost. */
static bool same_observation(yyjson_val *public_root, yyjson_val *private_root) {
    if (jget_int(public_root, "attempt", -1) != jget_int(private_root, "attempt", -2) ||
        !same(jget_str(public_root, "state"), jget_str(private_root, "state")))
        return false;
    yyjson_val *public_items = jget(public_root, "items");
    yyjson_val *private_items = jget(private_root, "items");
    if (!yyjson_is_arr(public_items) || !yyjson_is_arr(private_items) ||
        yyjson_arr_size(public_items) != yyjson_arr_size(private_items))
        return false;
    size_t count = yyjson_arr_size(public_items);
    for (size_t i = 0; i < count; ++i) {
        yyjson_val *public_item = yyjson_arr_get(public_items, i);
        yyjson_val *private_item = yyjson_arr_get(private_items, i);
        if (jget_int(public_item, "attempt", -1) != jget_int(private_item, "attempt", -2) ||
            !same(jget_str(public_item, "state"), jget_str(private_item, "state")))
            return false;
    }
    return true;
}

/* Only bounded, confined session JSON is read. The stored answer hash, not
 * model prose or the log's apparent ending, determines result integrity. */
static int collect(tny_ctx *ctx, yyjson_val *args, yyjson_val *root, buf_t *out, char *err,
                   size_t n) {
    int index = (int)jget_int(args, "item", -1);
    yyjson_val *item = yyjson_arr_get(jget(root, "items"), (size_t)index);
    const char *id = jget_str(root, "id");
    const char *state = jget_str(item, "state");
    size_t bound = (size_t)jget_int(args, "max_bytes", 16384);
    if (!terminal(state))
        return fail(err, n, "task is not terminal; use notifications or bounded wait-any");
    char *dir = run_dir(ctx, id);
    buf_t log = {0};
    char *log_path = recorded_state_path(ctx, jget_str(item, "log_path"));
    int log_rc =
        dir && log_path ? tny_image_io_read_confined(dir, log_path, TNY_JOBS_LOG_MAX, &log) : -1;
    free(log_path);
    free(dir);
    if (log_rc) {
        buf_free(&log);
        return fail(err, n, "task log is unavailable or exceeds the stored-log bound");
    }
    char log_sha[65];
    if (!tny_image_io_sha256_hex(log.data, log.len, log_sha) ||
        (same(state, "succeeded") && !same(log_sha, jget_str(item, "log_sha256")))) {
        buf_free(&log);
        return fail(err, n, "task log integrity changed");
    }
    const char *workspace = jget_str(item, "workspace_cwd");
    if (!workspace) workspace = jget_str(root, "workspace");
    yyjson_val *resolved = jget(item, "workspace");
    if (resolved) {
        workspace = jget_str(resolved, "cwd");
        if (!workspace) workspace = jget_str(resolved, "path");
    } else if (!jget_str(item, "workspace_cwd") && jget(jget(item, "request"), "workspace")) {
        buf_free(&log);
        return fail(err, n, "task workspace policy has no resolved execution cwd");
    }
    const char *sid = jget_str(item, "session_id");
    yyjson_doc *session = NULL;
    buf_t data = {0};
    const char *answer = NULL;
    size_t answer_len = 0;
    char answer_sha[65] = "";
    if (workspace && workspace[0] == '/' && session_id_valid(sid)) {
        tny_ctx location = *ctx;
        location.cwd = (char *)workspace;
        snprintf(location.ws_hash, sizeof location.ws_hash, "%016llx",
                 (unsigned long long)fnv1a(workspace, strlen(workspace)));
        char *sessions = state_path(ctx, "sessions");
        char *ws = sessions ? path_join(sessions, location.ws_hash) : NULL;
        char *sd = ws ? path_join(ws, sid) : NULL;
        char *path = sd ? path_join(sd, "session.json") : NULL;
        if (!session_is_running(&location, sid) && path && sessions &&
            tny_image_io_read_confined(sessions, path, TNY_TEAM_SESSION_MAX, &data) == 0)
            session = jparse(data.data, data.len);
        free(sessions);
        free(ws);
        free(sd);
        free(path);
        yyjson_val *sr = session ? yyjson_doc_get_root(session) : NULL;
        const char *status = jget_str(sr, "status");
        if (sr && (!status || same(status, "done"))) {
            size_t i, max;
            yyjson_val *message;
            yyjson_arr_foreach(jget(sr, "messages"), i, max, message) {
                if (same(jget_str(message, "role"), "assistant") &&
                    yyjson_is_str(jget(message, "content"))) {
                    answer = jget_strn(message, "content", &answer_len);
                }
            }
        }
    }
    bool integrity = answer && tny_image_io_sha256_hex(answer, answer_len, answer_sha) &&
                     same(answer_sha, jget_str(item, "result_sha256"));
    envelope(out, "team_collection", id);
    buf_appendf(out, ",\"attempt\":%lld,\"item\":%d,\"item_attempt\":%lld,\"execution\":",
                (long long)jget_int(root, "attempt", 0), index,
                (long long)jget_int(item, "attempt", 0));
    nullable_string(out, state);
    buf_appends(out, ",\"session_id\":");
    nullable_string(out, sid);
    buf_appends(out, ",\"result_integrity\":");
    nullable_string(out, integrity ? "matched" : "unavailable_or_changed");
    buf_appends(out, ",\"result_sha256\":");
    nullable_string(out, integrity ? answer_sha : NULL);
    size_t shown = integrity ? (answer_len < bound ? answer_len : bound) : 0;
    buf_appendf(out, ",\"result_bytes\":%zu,\"result_truncated\":%s,\"result_base64\":\"",
                integrity ? answer_len : 0, integrity && shown < answer_len ? "true" : "false");
    if (shown) b64_encode((const uint8_t *)answer, shown, out);
    buf_appendf(out, "\",\"log_bytes\":%zu,\"log_truncated\":%s,\"log_sha256\":", log.len,
                log.len > bound ? "true" : "false");
    nullable_string(out, log_sha);
    buf_appends(out, ",\"log_base64\":\"");
    size_t tail = log.len > bound ? bound : log.len;
    if (tail) b64_encode((const uint8_t *)log.data + log.len - tail, tail, out);
    buf_appends(out, "\"}\n");
    yyjson_doc_free(session);
    buf_free(&data);
    buf_free(&log);
    return finish(out, err, n, same(state, "succeeded") && integrity ? 0 : 2);
}

int tny_team_run(tny_ctx *ctx, const tny_team_caller *caller, tny_team_op op, yyjson_val *args,
                 buf_t *out, char *err, size_t n, bool (*cancelled)(void *), void *cancel_ud) {
    yyjson_doc *record = NULL;
    int rc = preflight(ctx, caller, op, args, &record, err, n);
    if (rc) {
        yyjson_doc_free(record);
        return rc;
    }
    if (cancelled && cancelled(cancel_ud)) {
        yyjson_doc_free(record);
        return 130;
    }
    if (op == TNY_TEAM_VERIFY) {
        yyjson_doc_free(record);
        envelope(out, "team_verification", jget_str(args, "id"));
        buf_appends(out, ",\"error_code\":\"TEAM_VERIFY_UNSUPPORTED\"}\n");
        return fail(err, n,
                    "verification execution is unavailable: strict check-process "
                    "cleanup/provenance is not implemented; work remains unverified");
    }
    if (op == TNY_TEAM_START) {
        buf_t job = {0};
        rc = tny_jobs_run_context(ctx, TNY_JOBS_OP_SUBMIT, args, &job, err, n, cancelled, cancel_ud,
                                  caller->local_operator ? NULL : caller->session_id);
        yyjson_doc *doc = job.len ? jparse(job.data, job.len) : NULL;
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        if (root) {
            envelope(out, "team", jget_str(root, "id"));
            buf_appends(out, ",\"job\":");
            buf_append(out, job.data, job.len);
            buf_appends(out, "}\n");
        }
        yyjson_doc_free(doc);
        buf_free(&job);
        return finish(out, err, n, rc);
    }
    if (op == TNY_TEAM_COLLECT) {
        yyjson_val *root = yyjson_doc_get_root(record);
        int64_t attempt = jget_int(root, "attempt", 0);
        rc = collect(ctx, args, root, out, err, n);
        /* Collection may read several bounded artifacts. Do not deliver them
         * under a membership/attempt that changed during that read window. */
        yyjson_doc *fresh = NULL;
        if (out->len &&
            (preflight(ctx, caller, op, args, &fresh, err, n) ||
             jget_int(fresh ? yyjson_doc_get_root(fresh) : NULL, "attempt", -1) != attempt)) {
            buf_clear(out);
            rc = fail(err, n, "team attempt or membership changed during collection");
        }
        yyjson_doc_free(fresh);
        yyjson_doc_free(record);
        if (cancelled && cancelled(cancel_ud)) {
            buf_clear(out);
            return 130;
        }
        return rc;
    }
    int64_t fence = jget_int(yyjson_doc_get_root(record), "attempt", 0);
    yyjson_doc_free(record);
    if (op == TNY_TEAM_CANCEL) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
        yyjson_mut_val *r = d ? yyjson_val_mut_copy(d, args) : NULL;
        if (!r) {
            yyjson_mut_doc_free(d);
            return fail(err, n, "allocation failed");
        }
        yyjson_mut_doc_set_root(d, r);
        if (jget(args, "item")) {
            yyjson_mut_val *indices = yyjson_mut_arr(d);
            if (!indices || !yyjson_mut_arr_add_int(d, indices, jget_int(args, "item", 0)) ||
                !yyjson_mut_obj_add_val(d, r, "items", indices)) {
                yyjson_mut_doc_free(d);
                return fail(err, n, "allocation failed");
            }
            yyjson_mut_obj_remove_key(r, "item");
        }
        char *json = jwrite(d);
        yyjson_doc *request = json ? jparse(json, strlen(json)) : NULL;
        if (!request) rc = fail(err, n, "allocation failed");
        else if (caller->run_id && caller->capability)
            rc = tny_jobs_cancel_member(ctx, yyjson_doc_get_root(request), out, err, n);
        else
            rc = tny_jobs_run_cancel(ctx, TNY_JOBS_OP_CANCEL, yyjson_doc_get_root(request), out,
                                     err, n, cancelled, cancel_ud);
        free(json);
        yyjson_doc_free(request);
        yyjson_mut_doc_free(d);
        return finish(out, err, n, rc);
    }
    int timeout_ms = (int)jget_int(args, "timeout_ms", 0);
    int64_t deadline = monotonic_ms() + timeout_ms;
    tny_jobs_watch watch = {.fd = -1, .directory_fd = -1};
    bool watching = op == TNY_TEAM_WAIT_ANY && timeout_ms > 0;
    if (watching) {
        if (!tny_jobs_host_watch_supported())
            return fail(err, n, "positive team waits require native directory notifications");
        char *dir = run_dir(ctx, jget_str(args, "id"));
        int watch_rc = dir ? tny_jobs_host_watch_open(dir, &watch) : -1;
        free(dir);
        if (watch_rc) return fail(err, n, "team completion notification watch could not be opened");
    }
    bool final_snapshot = false;
    for (;;) {
        if (cancelled && cancelled(cancel_ud)) {
            tny_jobs_host_watch_close(&watch);
            return 130;
        }
        /* Subscribe precedes the first snapshot. Drain before every public
         * snapshot so a change after this point remains queued until next(). */
        if (watching && tny_jobs_host_watch_drain(&watch)) {
            tny_jobs_host_watch_close(&watch);
            return fail(err, n, "team completion notification watch was lost");
        }
        /* jobs_status remains the sole abandoned-owner projection authority. */
        buf_t job = {0};
        rc = tny_jobs_run_cancel(ctx, TNY_JOBS_OP_STATUS, args, &job, err, n, cancelled, cancel_ud);
        yyjson_doc *status = job.len ? jparse(job.data, job.len) : NULL;
        yyjson_val *root = status ? yyjson_doc_get_root(status) : NULL;
        if (cancelled && cancelled(cancel_ud)) {
            yyjson_doc_free(status);
            buf_free(&job);
            tny_jobs_host_watch_close(&watch);
            return 130;
        }
        /* Clear notifications caused by the status read itself. The following
         * private snapshot closes the race created by that drain. */
        if (watching && tny_jobs_host_watch_drain(&watch)) {
            yyjson_doc_free(status);
            buf_free(&job);
            tny_jobs_host_watch_close(&watch);
            return fail(err, n, "team completion notification watch was lost");
        }
        /* The public projection intentionally omits private capability
         * verifiers. Revalidate against the confined current record, not a
         * projection that cannot authenticate a still-running member. */
        yyjson_doc *current = root ? read_record(ctx, jget_str(args, "id"), err, n) : NULL;
        yyjson_val *private_root = current ? yyjson_doc_get_root(current) : NULL;
        if (cancelled && cancelled(cancel_ud)) {
            yyjson_doc_free(current);
            yyjson_doc_free(status);
            buf_free(&job);
            tny_jobs_host_watch_close(&watch);
            return 130;
        }
        bool authorized = root && private_root && authority(caller, private_root, err, n) != -2 &&
                          jget_int(root, "attempt", 0) == fence &&
                          jget_int(private_root, "attempt", 0) == fence;
        if (!authorized) {
            yyjson_doc_free(current);
            yyjson_doc_free(status);
            buf_free(&job);
            tny_jobs_host_watch_close(&watch);
            return fail(err, n, "team attempt or membership changed while observing");
        }
        if (op == TNY_TEAM_STATUS) {
            envelope(out, "team", jget_str(root, "id"));
            buf_appends(out, ",\"job\":");
            buf_append(out, job.data, job.len);
            buf_appends(out, "}\n");
            yyjson_doc_free(current);
            yyjson_doc_free(status);
            buf_free(&job);
            tny_jobs_host_watch_close(&watch);
            return finish(out, err, n, rc);
        }
        if (watching && !same_observation(root, private_root)) {
            yyjson_doc_free(current);
            yyjson_doc_free(status);
            buf_free(&job);
            continue;
        }
        yyjson_doc_free(current);
        size_t i, max;
        yyjson_val *item;
        yyjson_arr_foreach(jget(root, "items"), i, max, item) {
            if ((jget(args, "item") && (int64_t)i != jget_int(args, "item", -1)) ||
                !terminal(jget_str(item, "state")) ||
                already_seen(args, (int)i, jget_int(item, "attempt", 0)))
                continue;
            if (cancelled && cancelled(cancel_ud)) {
                yyjson_doc_free(status);
                buf_free(&job);
                tny_jobs_host_watch_close(&watch);
                return 130;
            }
            envelope(out, "team_completion", jget_str(root, "id"));
            buf_appendf(out,
                        ",\"attempt\":%lld,\"cursor\":{\"item\":%zu,\"attempt\":%lld},\"item\":",
                        (long long)fence, i, (long long)jget_int(item, "attempt", 0));
            char *json = jwrite_val(item);
            if (json) buf_appends(out, json);
            else out->oom = true;
            free(json);
            buf_appends(out, "}\n");
            yyjson_doc_free(status);
            buf_free(&job);
            tny_jobs_host_watch_close(&watch);
            return finish(out, err, n, 0);
        }
        yyjson_doc_free(status);
        buf_free(&job);
        if (cancelled && cancelled(cancel_ud)) {
            tny_jobs_host_watch_close(&watch);
            return 130;
        }
        if (!watching || final_snapshot) {
            envelope(out, "team_wait_timeout", jget_str(args, "id"));
            buf_appendf(out, ",\"attempt\":%lld,\"cancelled\":false}\n", (long long)fence);
            tny_jobs_host_watch_close(&watch);
            return finish(out, err, n, 124);
        }
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            /* A hint can wake on creation of an atomic-write temporary before
             * the final rename. Take one last drain/snapshot at every observed
             * deadline, not only when watch_next() itself reports timeout. */
            final_snapshot = true;
            continue;
        }
        int event = tny_jobs_host_watch_next(&watch, (int)remaining, cancelled, cancel_ud);
        if (event == 1) continue;
        if (event == 0) {
            /* Owner-lock release has no portable directory notification. The
             * final snapshot also closes an event arriving at the deadline. */
            final_snapshot = true;
            continue;
        }
        tny_jobs_host_watch_close(&watch);
        if (event == -2) return 130;
        return fail(err, n, "team completion notification watch was lost");
    }
}
