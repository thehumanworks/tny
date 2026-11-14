/* Standalone toolkit jobs: a versioned data boundary, no CLI/agent adapter.
 * All provider behavior stays in the same core services used by the CLI. */
#define TNY_BUILDING_LIBRARY 1
#include "tny/tny.h"
#include "core/image_service.h"
#include "core/optimise.h"
#include "core/perm.h"
#include "core/tools.h"
#include "lib/error.h"
#include "util/alloc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REQUEST_MAX (256u * 1024u)

enum { JOB_READY, JOB_RUNNING, JOB_DONE };
enum { GENERATE, EDIT, OPTIMISE, LIST_TOOLS, CALL_TOOL, OPERATION_COUNT };
struct tny_toolkit_job {
    pid_t pid;
    atomic_int state;
    atomic_bool cancelled;
    yyjson_doc *doc;
    int operation;
    buf_t result;
    /* Private: the buffer holds a complete, locally constructed strict-image
     * failure object rather than a success result. Never part of the ABI. */
    bool image_detail;
    int32_t image_failure_status; /* exact local strict-size status, never diagnostic text */
    /* Private: the paid artifact is on disk and only its manifest failed, so
     * this failure outranks a late cancellation (ADR 0095). */
    bool image_committed;
};

static const char *const operations[] = {"generate_image", "edit_image", "optimise", "list_tools",
                                         "call_tool"};

/* Validate names as well as values. Never print user data in diagnostics. */
static bool fields(yyjson_val *object, const char *const *names, size_t count) {
    if (!yyjson_is_obj(object)) return false;
    uint64_t seen = 0;
    size_t index, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, index, max, key, value) {
        (void)value;
        const char *name = yyjson_get_str(key);
        if (!name || strlen(name) != yyjson_get_len(key)) return false;
        size_t i;
        for (i = 0; i < count; i++)
            if (!strcmp(name, names[i])) break;
        if (i == count || (seen & (UINT64_C(1) << i))) return false;
        seen |= UINT64_C(1) << i;
    }
    return true;
}

static bool string_value(yyjson_val *v, size_t max, bool required, bool secret) {
    if (!v) return !required;
    const char *s = yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    return s && n && n <= max && utf8_valid_bytes(s, n) && str_ws_prefix(s, n) != n &&
           (!secret || !strpbrk(s, "\r\n"));
}

static bool string_field(yyjson_val *o, const char *key, size_t max, bool required) {
    return string_value(jget(o, key), max, required, false);
}

static bool integer_field(yyjson_val *o, const char *key, uint64_t max, bool required) {
    yyjson_val *v = jget(o, key);
    return v ? yyjson_is_uint(v) && yyjson_get_uint(v) >= 1 && yyjson_get_uint(v) <= max
             : !required;
}

static bool url_field(yyjson_val *o, const char *key) {
    yyjson_val *v = jget(o, key);
    if (!v) return true;
    const char *s = yyjson_get_str(v);
    if (!string_value(v, 4096, true, true)) return false;
    const char *host = str_starts(s, "https://") ? s + 8 : str_starts(s, "http://") ? s + 7 : NULL;
    return host && *host && *host != '/' && !strpbrk(host, " \t?#@\\");
}

static bool request_valid(tny_toolkit_job *job) {
    static const char *const envelope[] = {"version", "operation", "config", "request"};
    static const char *const config[] = {"workspace", "settings_path", "chatgpt_token",
                                         "chatgpt_account_id", "codex_base_url"};
    /* Generate accepts the first nine names; edit adds the two reference
     * forms. Adding a name here is the deliberate act that lets an SDK option
     * through, so nothing new is silently discarded or silently accepted. */
    static const char *const image[] = {
        "prompt",      "output_file",      "provider",      "model",    "quality", "size",
        "strict_size", "persist_manifest", "from_manifest", "artifact", "images"};
    static const char *const optimise[] = {"text",    "provider", "model",          "base_url",
                                           "api_key", "wire_api", "timeout_seconds"};
    /* Native tool calls (ADR 0223): the same nested catalog as `tny tool`. */
    static const char *const list_tools[] = {"permission_mode"};
    static const char *const call_tool[] = {"name", "arguments", "permission_mode"};
    yyjson_val *root = yyjson_doc_get_root(job->doc);
    if (!fields(root, envelope, 4)) return false;
    yyjson_val *version = jget(root, "version");
    if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1 ||
        !string_field(root, "operation", 32, true))
        return false;
    const char *op = jget_str(root, "operation");
    for (job->operation = 0; job->operation < OPERATION_COUNT; job->operation++)
        if (!strcmp(op, operations[job->operation])) break;
    if (job->operation == OPERATION_COUNT) return false;
    yyjson_val *c = jget(root, "config"), *r = jget(root, "request");
    if (!fields(c, config, 5) || !string_field(c, "workspace", 4096, true) ||
        *jget_str(c, "workspace") != '/' || !string_field(c, "settings_path", 4096, false) ||
        !string_value(jget(c, "chatgpt_token"), 16384, false, true) ||
        !string_value(jget(c, "chatgpt_account_id"), 1024, false, true) ||
        !url_field(c, "codex_base_url") || !string_field(r, "provider", 128, false))
        return false;
    switch (job->operation) {
    case GENERATE:
    case EDIT: {
        yyjson_val *strict = jget(r, "strict_size"), *persist = jget(r, "persist_manifest");
        const char *source = jget_str(r, "from_manifest");
        const char *artifact = jget_str(r, "artifact");
        if (!fields(r, image, job->operation == EDIT ? 11 : 9) ||
            /* A rerun supplies the recorded prompt; anything else needs one. */
            !string_field(r, "prompt", TNY_IMAGE_PROMPT_MAX, !source) ||
            !string_field(r, "output_file", 4096, true) || !string_field(r, "model", 128, false) ||
            !string_field(r, "quality", 16, false) || !string_field(r, "size", 32, false) ||
            !string_field(r, "from_manifest", 4096, false) ||
            !string_field(r, "artifact", 4096, false) || (strict && !yyjson_is_bool(strict)) ||
            (persist && !yyjson_is_bool(persist)))
            return false;
        const char *quality = jget_str(r, "quality");
        if (quality && strcmp(quality, "auto") != 0 && strcmp(quality, "low") != 0 &&
            strcmp(quality, "medium") != 0 && strcmp(quality, "high") != 0 &&
            strcmp(quality, "xhigh") != 0 && strcmp(quality, "max") != 0)
            return false;
        yyjson_val *images = jget(r, "images");
        /* A rerun carries its own references; mixing in new ones or a second
         * record would leave the operation ambiguous. */
        if (source && (artifact || images)) return false;
        if (job->operation == GENERATE) return !artifact;
        if (source) return true;
        size_t count = yyjson_arr_size(images) + (artifact ? 1u : 0u);
        if (images && !yyjson_is_arr(images)) return false;
        if (!count || count > 5) return false;
        size_t i, n;
        yyjson_val *v;
        yyjson_arr_foreach(images, i, n, v) if (!string_value(v, 4096, true, false)) return false;
        return true;
    }
    case OPTIMISE: {
        const char *wire = jget_str(r, "wire_api");
        const char *text = jget_str(r, "text");
        return fields(r, optimise, 7) && string_field(r, "text", TNY_OPTIMISE_TEXT_MAX, true) &&
               tny_prompt_text_valid(text, strlen(text)) && string_field(r, "model", 128, false) &&
               url_field(r, "base_url") && string_value(jget(r, "api_key"), 16384, false, true) &&
               string_field(r, "wire_api", 16, false) &&
               (!wire || !strcmp(wire, "chat") || !strcmp(wire, "responses")) &&
               integer_field(r, "timeout_seconds", 86400, false);
    }
    case LIST_TOOLS:
    case CALL_TOOL: {
        const char *mode = jget_str(r, "permission_mode");
        if (jget(r, "permission_mode") &&
            (!mode ||
             (strcmp(mode, "ask") != 0 && strcmp(mode, "auto") != 0 && strcmp(mode, "yolo") != 0)))
            return false;
        if (job->operation == LIST_TOOLS) return fields(r, list_tools, 1);
        yyjson_val *arguments = jget(r, "arguments");
        return fields(r, call_tool, 3) && string_field(r, "name", 128, true) &&
               (!arguments || yyjson_is_obj(arguments));
    }
    default: return false;
    }
}

static bool stopped(void *opaque) {
    tny_toolkit_job *job = opaque;
    return atomic_load(&job->cancelled);
}

static char *workspace_path(const tny_ctx *ctx, const char *path) {
    return !path ? NULL : *path == '/' ? xstrdup(path) : path_join(ctx->cwd, path);
}

static int run_image(tny_toolkit_job *job, tny_ctx *ctx, yyjson_val *r, char *err, size_t len) {
    char *output = workspace_path(ctx, jget_str(r, "output_file"));
    char *source = workspace_path(ctx, jget_str(r, "from_manifest"));
    yyjson_val *persist = jget(r, "persist_manifest");
    tny_image_request req = {.edit = job->operation == EDIT,
                             .prompt = jget_str(r, "prompt"),
                             .provider = jget_str(r, "provider"),
                             .model = jget_str(r, "model"),
                             .quality = jget_str(r, "quality"),
                             .size = jget_str(r, "size"),
                             .output_file = output,
                             .strict_size = yyjson_get_bool(jget(r, "strict_size")),
                             .no_manifest = persist && !yyjson_get_bool(persist),
                             .from_manifest = source,
                             .cancelled = stopped,
                             .userdata = job};
    const char *artifact = jget_str(r, "artifact");
    if (artifact) {
        /* Declared first, ahead of any explicit reference paths. */
        req.image_is_artifact[req.image_count] = true;
        req.images[req.image_count++] = workspace_path(ctx, artifact);
    }
    yyjson_val *images = jget(r, "images");
    for (size_t i = 0; i < yyjson_arr_size(images); i++)
        req.images[req.image_count++] =
            workspace_path(ctx, yyjson_get_str(yyjson_arr_get(images, i)));
    tny_image_result result = {0}; /* readable even if the run never starts */
    int rc = tny_alloc_scope_failed() ? 1 : tny_image_run(ctx, &req, &result, err, len);
    if (!rc) tny_image_result_json(&req, &result, &job->result);
    /* The artifact is committed and exactly tny's manifest finalization
     * failed: an I/O outcome that still names the file that was kept. */
    else if (tny_image_retained_failure(&result)) {
        job->image_committed = true;
        job->image_failure_status = TNY_STATUS_IO;
        tny_image_retained_json(&req, &result, err, &job->result);
        if (job->result.oom) {
            if (job->result.data) secure_zero(job->result.data, job->result.len);
            buf_clear(&job->result);
        } else job->image_detail = true;
    }
    /* The code is what selects this path, so name that precondition here
     * rather than leaving it implied by the predicate in another unit. */
    else if (result.code && tny_image_strict_failure(&result)) {
        job->image_failure_status = strcmp(result.code, TNY_IMAGE_CODE_STRICT_INVALID) == 0
                                        ? TNY_STATUS_INVALID_ARGUMENT
                                        : TNY_STATUS_PROTOCOL;
        /* Only tny's own strict-size decision may leave a failure result, and
         * only once it is complete: a truncated object is dropped, and run()
         * turns the same exhaustion into a plain out-of-memory failure. */
        tny_image_error_json(&req, &result, err, &job->result);
        if (job->result.oom) {
            if (job->result.data) secure_zero(job->result.data, job->result.len);
            buf_clear(&job->result);
        } else job->image_detail = true;
    }
    for (size_t i = 0; i < req.image_count; i++) free((void *)req.images[i]);
    free(source);
    free(output);
    return rc;
}

static tny_perm_mode perm_mode_named(const char *s, tny_perm_mode fallback) {
    if (!s) return fallback;
    if (!strcmp(s, "ask")) return TNY_MODE_ASK;
    if (!strcmp(s, "auto")) return TNY_MODE_AUTO;
    if (!strcmp(s, "yolo")) return TNY_MODE_YOLO;
    return fallback;
}

/* One embedded nested-tool operation. The context keeps library_mode, so
 * process-owning and ambient-state tools (terminal, subagent, jobs, memory,
 * skills, MCP, extensions) stay out of the catalog exactly as for embedded
 * runtimes (ADR 0215); turn-bound tools are hidden and refused as in
 * `tny tool` (ADR 0217). An explicit request mode wins, then settings, then
 * yolo: the caller naming a tool is the decision a reviewer would make. */
static int run_tool(tny_toolkit_job *job, tny_ctx *ctx, yyjson_val *r) {
    yyjson_val *sroot = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    const char *configured = jget_str(jget(jget(sroot, "workspaces"), ctx->cwd), "permission_mode");
    if (!configured) configured = jget_str(sroot, "permission_mode");
    ctx->perm_mode =
        perm_mode_named(jget_str(r, "permission_mode"), perm_mode_named(configured, TNY_MODE_YOLO));
    perm_engine *perm = perm_new(ctx);
    if (!perm) return 1;
    tools_env env = {.ctx = ctx, .perm = perm, .cancelled = stopped, .cancelled_ud = job};
    int rc = 1;
    if (job->operation == LIST_TOOLS) {
        char *catalog = tools_catalog_json(&env);
        yyjson_doc *doc = catalog ? jparse(catalog, strlen(catalog)) : NULL;
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        if (root && yyjson_is_arr(root)) {
            buf_appends(&job->result, "{\"tools\":[");
            size_t idx, max, n = 0;
            yyjson_val *item;
            yyjson_arr_foreach(root, idx, max, item) {
                yyjson_val *fn = jget(item, "function");
                if (tools_turn_bound(jget_str(fn, "name"))) continue;
                char *text = jwrite_val(fn);
                if (!text) {
                    job->result.oom = true;
                    break;
                }
                if (n++) buf_appends(&job->result, ",");
                buf_appends(&job->result, text);
                free(text);
            }
            buf_appends(&job->result, "]}");
            rc = 0;
        }
        yyjson_doc_free(doc);
        free(catalog);
    } else {
        const char *name = jget_str(r, "name");
        yyjson_val *arguments = jget(r, "arguments");
        char *args = arguments ? jwrite_val(arguments) : xstrdup("{}");
        char *result = !args ? NULL
                       : tools_turn_bound(name)
                           ? tool_err("%s belongs to a conversation turn", name)
                           : tools_execute(&env, name, args);
        if (result) {
            bool ok = !str_starts(result, "error:");
            buf_appends(&job->result, "{\"name\":");
            jescape(&job->result, name);
            buf_appendf(&job->result, ",\"ok\":%s,\"result\":", ok ? "true" : "false");
            jescape(&job->result, result);
            buf_appends(&job->result, "}");
            /* A call stopped part way is a cancellation, not a tool error. */
            rc = !ok && stopped(job) ? 130 : 0;
        }
        free(result);
        free(args);
    }
    tools_discard_pending_images(&env);
    perm_free(perm);
    return rc;
}

static int32_t run(tny_toolkit_job *job) {
    if (stopped(job)) return TNY_STATUS_CANCELLED;
    /* Public embedding has no matching execution-server launcher. Re-execing
     * Python/Node as tny is invalid, and workspace exploration cannot fall
     * back to in-process tools. Refuse before credentials or provider IO. */
    if (job->operation == OPTIMISE) return TNY_STATUS_UNSUPPORTED;
    yyjson_val *root = yyjson_doc_get_root(job->doc);
    yyjson_val *c = jget(root, "config"), *r = jget(root, "request");
    char *state_dir = path_tny_dir();
    tny_ctx *ctx = state_dir ? tny_ctx_new_explicit(jget_str(c, "workspace"), state_dir) : NULL;
    free(state_dir);
    if (!ctx) return TNY_STATUS_CONFIG;
    char *settings = workspace_path(ctx, jget_str(c, "settings_path"));
    ctx->settings = jparse_file(settings ? settings : ctx->settings_path);
    bool settings_error = settings && !ctx->settings;
    free(settings);
    ctx->chatgpt_token =
        jget_str(c, "chatgpt_token") ? xstrdup(jget_str(c, "chatgpt_token")) : NULL;
    ctx->chatgpt_account_id =
        jget_str(c, "chatgpt_account_id") ? xstrdup(jget_str(c, "chatgpt_account_id")) : NULL;
    ctx->codex_base_url =
        jget_str(c, "codex_base_url") ? xstrdup(jget_str(c, "codex_base_url")) : NULL;
    int rc = 1;
    char err[512] = "";
    if (!settings_error && !tny_alloc_scope_failed() && !stopped(job)) {
        switch (job->operation) {
        case GENERATE:
        case EDIT: rc = run_image(job, ctx, r, err, sizeof err); break;
        case LIST_TOOLS:
        case CALL_TOOL: rc = run_tool(job, ctx, r); break;
        case OPTIMISE: break; /* rejected before creating a context */
        }
    }
    tny_ctx_free(ctx);
    if (tny_alloc_scope_failed() || job->result.oom) return TNY_STATUS_OOM;
    /* A committed artifact whose record failed is an I/O failure, decided
     * before cancellation is even consulted: the file exists, so reporting
     * "cancelled" and dropping its detail would hide paid work (ADR 0095). */
    if (job->image_committed && job->image_failure_status) return job->image_failure_status;
    /* A completed artifact wins a late cancellation: never report cancellation
     * after a successful atomic rename. All services check before committing. */
    if (!rc) return TNY_STATUS_OK;
    if (rc == 130 || stopped(job)) return TNY_STATUS_CANCELLED;
    /* Strict-size outcomes are tny's own decision, never provider text: an
     * unusable size request is an argument error, and returned bytes that miss
     * the required dimensions are a protocol-level rejection. */
    if (job->image_failure_status) return job->image_failure_status;
    if (strstr(err, "timed out") || strstr(err, "timeout")) return TNY_STATUS_TIMEOUT_ERROR;
    if (strstr(err, "login") || strstr(err, "credential") || strstr(err, "API key"))
        return TNY_STATUS_AUTH;
    if (strstr(err, "unknown") || strstr(err, "unavailable") || strstr(err, "requires a native"))
        return TNY_STATUS_UNSUPPORTED;
    if (rc == 2 || strstr(err, "invalid") || strstr(err, "incomplete") || strstr(err, "empty") ||
        strstr(err, "exceeds"))
        return TNY_STATUS_PROTOCOL;
    return settings_error ? TNY_STATUS_CONFIG : TNY_STATUS_IO;
}

int32_t tny_toolkit_job_create(tny_bytes json, tny_toolkit_job **out, tny_error **error) {
    tny_alloc_scope_begin("toolkit_create");
    if (error) *error = NULL;
    if (out) *out = NULL;
    if (!out || !json.ptr || !json.len || json.len > REQUEST_MAX ||
        !utf8_valid_bytes(json.ptr, (size_t)json.len))
        return tny_lib_error(error, TNY_STATUS_INVALID_ARGUMENT,
                             "toolkit needs UTF-8 JSON of at most 256 KiB");
    tny_toolkit_job *job = calloc(1, sizeof *job);
    if (!job) return tny_lib_error(error, TNY_STATUS_OOM, "out of memory");
    job->pid = getpid();
    atomic_init(&job->state, JOB_READY);
    atomic_init(&job->cancelled, false);
    job->doc = jparse(json.ptr, (size_t)json.len);
    bool valid = job->doc && request_valid(job);
    bool oom = tny_alloc_scope_failed();
    if (!valid || oom) {
        (void)tny_toolkit_job_destroy(&job);
        return tny_lib_error(error, oom ? TNY_STATUS_OOM : TNY_STATUS_INVALID_ARGUMENT,
                             oom ? "out of memory"
                                 : "invalid version-1 toolkit request; see docs/sdk-toolkit.md");
    }
    *out = job;
    return TNY_STATUS_OK;
}

/* Wipe and drop a staged result unless a complete local image-failure object —
 * a strict-size rejection or a retained artifact — is allowed to survive this
 * outcome. Successful results never reach this. */
static void clear_result(tny_toolkit_job *job, bool final_override) {
    if (job->image_detail && !final_override) return;
    job->image_detail = false;
    /* Clearing the detail also clears the claim that one was committed; the
     * artifact itself is never touched. */
    job->image_committed = false;
    if (job->result.data) secure_zero(job->result.data, job->result.len);
    buf_clear(&job->result);
}

int32_t tny_toolkit_job_run(tny_toolkit_job *job, tny_error **error) {
    tny_alloc_scope_begin("toolkit_run");
    if (error) *error = NULL;
    if (!job) return tny_lib_error(error, TNY_STATUS_INVALID_ARGUMENT, "toolkit job is required");
    if (job->pid != getpid()) return TNY_STATUS_BAD_STATE;
    int ready = JOB_READY;
    if (!atomic_compare_exchange_strong(&job->state, &ready, JOB_RUNNING))
        return tny_lib_error(error, TNY_STATUS_BAD_STATE, "toolkit job is single-use");
    int32_t status = run(job);
    if (tny_alloc_scope_failed()) status = TNY_STATUS_OOM;
    if (status != TNY_STATUS_OK) {
        /* A complete strict-size or retained-artifact object is tny's own safe
         * decision and stays readable through the existing accessor. A status
         * of cancelled or exhausted always wins: those outcomes leave no
         * result at all — and a committed artifact never becomes either. */
        clear_result(job, status == TNY_STATUS_CANCELLED || status == TNY_STATUS_OOM);
        /* Provider errors can contain prompts and credentials. Expose only a
         * stable category through SDK exceptions, never their response body. */
        status = tny_lib_error(
            error, status,
            status == TNY_STATUS_CANCELLED ? "toolkit operation cancelled"
            : status == TNY_STATUS_AUTH    ? "toolkit provider credentials are missing or rejected"
            : status == TNY_STATUS_UNSUPPORTED && job->operation == OPTIMISE
                ? "embedded prompt optimisation requires an execution-server launcher; no direct "
                  "fallback"
            : status == TNY_STATUS_UNSUPPORTED
                ? "toolkit provider or host capability is unavailable"
            : status == TNY_STATUS_CONFIG        ? "toolkit workspace or settings are unavailable"
            : status == TNY_STATUS_TIMEOUT_ERROR ? "toolkit operation timed out"
            : status == TNY_STATUS_OOM           ? "out of memory"
                                                 : "toolkit operation failed");
        /* Building that error can itself exhaust the scope and rewrite the
         * status; the same rule then applies to the staged detail. */
        clear_result(job, status == TNY_STATUS_CANCELLED || status == TNY_STATUS_OOM);
    }
    atomic_store(&job->state, JOB_DONE);
    return status;
}

int32_t tny_toolkit_job_cancel(tny_toolkit_job *job) {
    if (!job) return TNY_STATUS_INVALID_ARGUMENT;
    if (job->pid != getpid()) return TNY_STATUS_BAD_STATE;
    atomic_store(&job->cancelled, true);
    return TNY_STATUS_OK;
}

tny_bytes tny_toolkit_job_result(const tny_toolkit_job *job) {
    if (!job || job->pid != getpid() || atomic_load(&job->state) != JOB_DONE) return (tny_bytes){0};
    return (tny_bytes){job->result.data, job->result.len};
}

int32_t tny_toolkit_job_destroy(tny_toolkit_job **ptr) {
    if (!ptr) return TNY_STATUS_INVALID_ARGUMENT;
    tny_toolkit_job *job = *ptr;
    if (!job) return TNY_STATUS_OK;
    if (job->pid != getpid()) return TNY_STATUS_BAD_STATE;
    if (atomic_load(&job->state) == JOB_RUNNING) return TNY_STATUS_BUSY;
    /* yyjson owns decoded strings. Wipe credentials and private prompt
     * material before freeing the document. */
    if (job->doc) {
        yyjson_val *v = yyjson_doc_get_root(job->doc);
        size_t count = yyjson_doc_get_val_count(job->doc);
        for (size_t i = 0; i < count; i++)
            if (yyjson_is_str(v + i))
                secure_zero((void *)yyjson_get_str(v + i), yyjson_get_len(v + i));
        yyjson_doc_free(job->doc);
    }
    if (job->result.data) secure_zero(job->result.data, job->result.len);
    buf_free(&job->result);
    free(job);
    *ptr = NULL;
    return TNY_STATUS_OK;
}
