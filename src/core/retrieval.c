#include "core/retrieval.h"
#include "core/agent_presets.h"
#include "core/retrieval_sources.h"
#include "core/tasks.h"
#include "core/subagent.h"
#include "util/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *write_value(yyjson_val *value) {
    return value ? yyjson_val_write(value, 0, NULL) : NULL;
}

static yyjson_doc *saved_state(tny_session_state *s, bool *present) {
    yyjson_mut_val *value =
        s ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "retrieval") : NULL;
    if (present) *present = value != NULL;
    char *text = value ? yyjson_mut_val_write(value, 0, NULL) : NULL;
    yyjson_doc *doc = text ? jparse(text, strlen(text)) : NULL;
    free(text);
    return doc;
}

static bool bounded_text(yyjson_val *value, size_t max) {
    return yyjson_is_str(value) && yyjson_get_len(value) <= max &&
           strlen(yyjson_get_str(value)) == yyjson_get_len(value) &&
           utf8_valid_bytes(yyjson_get_str(value), yyjson_get_len(value));
}

static bool object_keys(yyjson_val *object, const char *const *keys, size_t count) {
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != count) return false;
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, i, n, key, value) {
        if (!bounded_text(key, 32)) return false;
        const char *name = yyjson_get_str(key);
        if (!name || jget(object, name) != value) return false;
        bool known = false;
        for (size_t k = 0; k < count; ++k)
            if (strcmp(keys[k], name) == 0) known = true;
        if (!known) return false;
    }
    return true;
}

static bool hex_field(yyjson_val *value, size_t length) {
    if (!bounded_text(value, length) || yyjson_get_len(value) != length) return false;
    const char *text = yyjson_get_str(value);
    for (; *text; ++text)
        if (!((*text >= '0' && *text <= '9') || (*text >= 'a' && *text <= 'f'))) return false;
    return true;
}

/* Freeze routing identity, not credentials: token refresh remains normal. Only
 * digests enter durable state; secret-bearing endpoint strings are never saved
 * in the preset snapshot. Ordered field hashes keep separators unambiguous. */
static bool route_part(unsigned char state[32], const char *text) {
    unsigned char pair[64];
    memcpy(pair, state, 32);
    if (!sha256((const uint8_t *)(text ? text : ""), text ? strlen(text) : 0, pair + 32))
        return false;
    return sha256(pair, sizeof pair, state);
}

static bool routing_fingerprint(const tny_ctx *ctx, char out[65]) {
    unsigned char digest[32] = {0};
    const char *parts[] = {tny_provider_name(ctx), ctx->base_url, ctx->codex_base_url,
                           tny_provider_wire_is_chat(ctx) ? "chat" : "responses",
                           ctx->openai_signin ? "signin" : "key"};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i)
        if (!route_part(digest, parts[i])) return false;
    if (ctx->backend == TNY_BK_ACP && ctx->agent_argv)
        for (size_t i = 0; ctx->agent_argv[i]; ++i)
            if (!route_part(digest, ctx->agent_argv[i])) return false;
    for (size_t i = 0; i < sizeof digest; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return true;
}

static bool state_valid(yyjson_val *state) {
    yyjson_val *sources = jget(state, "sources");
    static const char *const state_keys[] = {"profile", "sources", "routing"};
    static const char *const source_keys[] = {"source", "handle", "digest", "coverage", "bytes"};
    if (!object_keys(state, state_keys, 3) || !hex_field(jget(state, "routing"), 64) ||
        !tny_agent_preset_snapshot_valid(jget(state, "profile")) || !yyjson_is_arr(sources) ||
        yyjson_arr_size(sources) > TNY_RETRIEVAL_SOURCE_LIMIT)
        return false;
    size_t i, n, total = 0;
    yyjson_val *entry;
    yyjson_arr_foreach(sources, i, n, entry) {
        yyjson_val *bytes = jget(entry, "bytes");
        if (!object_keys(entry, source_keys, 5) || !bounded_text(jget(entry, "source"), 256) ||
            !yyjson_get_len(jget(entry, "source")) || !hex_field(jget(entry, "handle"), 16) ||
            !hex_field(jget(entry, "digest"), 64) || !bounded_text(jget(entry, "coverage"), 32) ||
            !yyjson_is_uint(bytes) || yyjson_get_uint(bytes) > TNY_RETRIEVAL_BYTES_LIMIT - total)
            return false;
        total += (size_t)yyjson_get_uint(bytes);
    }
    return true;
}

static bool raw_selection(yyjson_val *args) {
    return jget(args, "provider") || jget(args, "model") || jget(args, "effort");
}

char *tny_retrieval_prepare(tools_env *env, yyjson_val *args, tny_session_state *child,
                            yyjson_doc **selection, char **error, bool *applies) {
    *selection = NULL;
    *error = NULL;
    const char *name = jget_str(args, "preset");
    bool present = false;
    yyjson_doc *stored = saved_state(child, &present);
    *applies = name || present;
    yyjson_val *state = stored ? yyjson_doc_get_root(stored) : NULL;
    if (present && !stored) {
        *error = tool_err("SUBAGENT_INVALID_RESPONSE: cannot read the pinned retrieval snapshot");
        return NULL;
    }
    if (!name && !present) {
        yyjson_doc_free(stored);
        return NULL;
    }
    if (!tny_process_tree_supported() || env->ctx->no_save || raw_selection(args) ||
        (child && jget(args, "sources")) ||
        (child && (!state_valid(state) ||
                   (name && strcmp(name, jget_str(jget(state, "profile"), "name")) != 0)))) {
        *error = tool_err("SUBAGENT_INVALID_ARGUMENT: preset children require saved sessions; "
                          "provider/model/effort are pinned, and sources are create-only");
        yyjson_doc_free(stored);
        return NULL;
    }
    char *resolved =
        child ? write_value(jget(state, "profile")) : tny_agent_preset_resolve(env->ctx, name);
    yyjson_doc *profile_doc = resolved ? jparse(resolved, strlen(resolved)) : NULL;
    free(resolved);
    yyjson_val *profile = profile_doc ? yyjson_doc_get_root(profile_doc) : NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (d) yyjson_mut_doc_set_root(d, root);
    yyjson_mut_val *sources = d ? yyjson_mut_arr(d) : NULL;
    bool ok = root && sources && tny_agent_preset_snapshot_valid(profile) &&
              yyjson_mut_obj_add_val(d, root, "profile", yyjson_val_mut_copy(d, profile)) &&
              yyjson_mut_obj_add_strcpy(d, root, "prompt", jget_str(args, "prompt")) &&
              yyjson_mut_obj_add_val(d, root, "sources", sources);
    if (ok && !child) {
        tny_ctx task_ctx = *env->ctx;
        task_ctx.task_name = NULL;
        task_ctx.task_source = NULL;
        task_ctx.task_instructions = NULL;
        task_ctx.task_digest[0] = 0;
        task_ctx.task_explicit = false;
        ok = tny_task_apply(&task_ctx, jget_str(profile, "task")) == 0 &&
             yyjson_mut_obj_add_strcpy(d, root, "task_body", task_ctx.task_instructions) &&
             yyjson_mut_obj_add_strcpy(d, root, "task_source", task_ctx.task_source);
        free(task_ctx.task_name);
        free(task_ctx.task_source);
        free(task_ctx.task_instructions);
    }
    yyjson_val *requested = jget(args, "sources");
    if (requested &&
        (!yyjson_is_arr(requested) || yyjson_arr_size(requested) > TNY_RETRIEVAL_SOURCE_LIMIT))
        ok = false;
    size_t index, count, total = 0;
    yyjson_val *item;
    if (ok && requested) {
        yyjson_arr_foreach(requested, index, count, item) {
            const char *handle = yyjson_is_str(item) ? yyjson_get_str(item) : NULL;
            if (!handle || strlen(handle) != yyjson_get_len(item)) {
                ok = false;
                break;
            }
            size_t bytes = 0;
            char *materialized = NULL;
            const char *artifact = handle;
            buf_t qualified;
            buf_init(&qualified);
            if (strncmp(handle, "transcript:message:", 19) == 0) {
                materialized = session_materialize_transcript(env->session, handle, &bytes);
                buf_appendf(&qualified, "transcript:artifact:%s:", env->session->id);
                if (!materialized || qualified.oom ||
                    strncmp(materialized, qualified.data, qualified.len) != 0) {
                    free(materialized);
                    buf_free(&qualified);
                    ok = false;
                    break;
                }
                artifact = materialized + qualified.len;
                buf_free(&qualified);
                buf_init(&qualified);
                buf_appends(&qualified, handle);
            } else buf_appendf(&qualified, "result:%s:%s", env->session->id, handle);
            yyjson_mut_val *entry =
                qualified.oom ? NULL
                              : tny_retrieval_source_export(d, env->session, artifact,
                                                            qualified.data, "unknown", &bytes);
            free(materialized);
            buf_free(&qualified);
            if (!entry || bytes > TNY_RETRIEVAL_BYTES_LIMIT - total ||
                !yyjson_mut_arr_append(sources, entry)) {
                ok = false;
                break;
            }
            total += bytes;
        }
    }
    char *payload = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    yyjson_doc_free(stored);
    if (payload && strlen(payload) <= TNY_RETRIEVAL_INPUT_MAX) {
        *selection = profile_doc;
        return payload;
    }
    free(payload);
    yyjson_doc_free(profile_doc);
    *error = tool_err("SUBAGENT_INVALID_ARGUMENT: preset, task, or source snapshot unavailable; "
                      "sources must be parent-owned retained handles within the 64 MiB total cap");
    return NULL;
}

/* Called before session creation or provider connection. The body is trusted
 * launch configuration, never a provider-returned tool argument or page text. */
int tny_retrieval_input(tny_ctx *ctx, buf_t *prompt, bool resuming, yyjson_doc **input) {
    *input = NULL;
    if (!ctx || !prompt || prompt->len > TNY_RETRIEVAL_INPUT_MAX) return -1;
    yyjson_doc *d = jparse(prompt->data, prompt->len);
    yyjson_val *root = d ? yyjson_doc_get_root(d) : NULL;
    yyjson_val *profile = jget(root, "profile");
    const char *question = jget_str(root, "prompt");
    const char *body = jget_str(root, "task_body");
    const char *source = jget_str(root, "task_source");
    static const char *const create_keys[] = {"profile", "prompt", "sources", "task_body",
                                              "task_source"};
    static const char *const resume_keys[] = {"profile", "prompt", "sources"};
    bool ok = object_keys(root, resuming ? resume_keys : create_keys, resuming ? 3 : 5) &&
              (resuming ? (!jget(root, "task_body") && !jget(root, "task_source") &&
                           yyjson_is_arr(jget(root, "sources")) &&
                           yyjson_arr_size(jget(root, "sources")) == 0)
                        : body != NULL) &&
              tny_agent_preset_snapshot_valid(profile) && question && *question &&
              bounded_text(jget(root, "prompt"), TNY_RETRIEVAL_INPUT_MAX) &&
              tny_subagent_provider_is_parent(ctx, jget_str(profile, "provider")) && ctx->model &&
              strcmp(ctx->model, jget_str(profile, "model")) == 0 &&
              yyjson_is_arr(jget(root, "sources")) &&
              yyjson_arr_size(jget(root, "sources")) <= TNY_RETRIEVAL_SOURCE_LIMIT;
    if (ok && body)
        ok = source && tny_task_set_explicit(ctx, jget_str(profile, "task"), body, source) == 0;
    if (!ok) {
        yyjson_doc_free(d);
        return -1;
    }
    buf_free(prompt);
    buf_init(prompt);
    buf_appends(prompt, question);
    if (prompt->oom) {
        yyjson_doc_free(d);
        return -1;
    }
    *input = d;
    return 0;
}

int tny_retrieval_import(tny_session_state *session, yyjson_doc *input, buf_t *prompt) {
    (void)prompt;
    if (!input) return 0;
    yyjson_val *root = yyjson_doc_get_root(input);
    yyjson_val *profile = jget(root, "profile");
    bool existing = false;
    yyjson_doc *previous = saved_state(session, &existing);
    yyjson_doc_free(previous);
    if (existing && jget(root, "task_body")) return -1;
    if (!jget(root, "task_body")) {
        yyjson_doc *saved = saved_state(session, NULL);
        yyjson_val *state = saved ? yyjson_doc_get_root(saved) : NULL;
        char *a = write_value(jget(state, "profile"));
        char *b = write_value(profile);
        bool ok = state_valid(state) && a && b && strcmp(a, b) == 0;
        free(a);
        free(b);
        yyjson_doc_free(saved);
        return ok ? 0 : -1;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *state = d ? yyjson_mut_obj(d) : NULL;
    if (d) yyjson_mut_doc_set_root(d, state);
    yyjson_mut_val *sources = d ? yyjson_mut_arr(d) : NULL;
    char routing[65];
    bool ok = state && sources && routing_fingerprint(session->ctx, routing) &&
              yyjson_mut_obj_add_strcpy(d, state, "routing", routing) &&
              yyjson_mut_obj_add_val(d, state, "profile", yyjson_val_mut_copy(d, profile)) &&
              yyjson_mut_obj_add_val(d, state, "sources", sources);
    /* Validate aggregate size before any source file is copied. */
    size_t i, n, total = 0;
    yyjson_val *entry;
    if (ok) yyjson_arr_foreach(jget(root, "sources"), i, n, entry) {
            yyjson_val *bytes = jget(entry, "bytes");
            if (!yyjson_is_uint(bytes) ||
                yyjson_get_uint(bytes) > TNY_RETRIEVAL_BYTES_LIMIT - total) {
                ok = false;
                break;
            }
            total += (size_t)yyjson_get_uint(bytes);
        }
    total = 0;
    if (ok) yyjson_arr_foreach(jget(root, "sources"), i, n, entry) {
            size_t bytes = 0;
            yyjson_mut_val *imported = tny_retrieval_source_import(d, session, entry, &bytes);
            if (!imported || bytes > TNY_RETRIEVAL_BYTES_LIMIT - total ||
                !yyjson_mut_arr_append(sources, imported)) {
                if (imported)
                    tny_retrieval_source_discard(
                        session, yyjson_mut_get_str(yyjson_mut_obj_get(imported, "handle")));
                ok = false;
                break;
            }
            total += bytes;
        }
    if (ok) {
        yyjson_mut_val *frozen = yyjson_mut_obj_get(state, "profile");
        const char *effort = session->ctx->reasoning_effort;
        ok = yyjson_mut_obj_put(frozen, yyjson_mut_strcpy(d, "effort"),
                                yyjson_mut_strcpy(d, effort && *effort ? effort : "default"));
    }
    char *text = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
    if (text) {
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(session->doc, state);
        ok = copy && yyjson_mut_obj_put(yyjson_mut_doc_get_root(session->doc),
                                        yyjson_mut_strcpy(session->doc, "retrieval"), copy);
        if (ok) {
            free(session->ctx->retrieval_state);
            session->ctx->retrieval_state = text;
        } else free(text);
    } else ok = false;
    if (!ok && sources) {
        size_t rollback_index, rollback_count;
        yyjson_mut_val *imported;
        yyjson_mut_arr_foreach(sources, rollback_index, rollback_count, imported)
            tny_retrieval_source_discard(
                session, yyjson_mut_get_str(yyjson_mut_obj_get(imported, "handle")));
    }
    yyjson_mut_doc_free(d);
    return ok ? 0 : -1;
}

int tny_retrieval_reconcile(tny_session_state *session, char *error, size_t error_size) {
    if (!session || !session->ctx) return -1;
    /* Session reload permits callers to omit diagnostic storage. */
    char ignored_error[256];
    if (!error || !error_size) {
        error = ignored_error;
        error_size = sizeof ignored_error;
    }
    bool present = false;
    yyjson_doc *stored = saved_state(session, &present);
    if (present && !stored) {
        snprintf(error, error_size, "cannot read the pinned retrieval snapshot");
        return -1;
    }
    if (!present) {
        free(session->ctx->retrieval_state);
        session->ctx->retrieval_state = NULL;
        return 0;
    }
    yyjson_val *state = yyjson_doc_get_root(stored);
    yyjson_val *profile = jget(state, "profile");
    char routing[65];
    bool ok = state_valid(state) && routing_fingerprint(session->ctx, routing) &&
              strcmp(routing, jget_str(state, "routing")) == 0 &&
              tny_subagent_provider_is_parent(session->ctx, jget_str(profile, "provider")) &&
              session->ctx->model && strcmp(session->ctx->model, jget_str(profile, "model")) == 0 &&
              strcmp(session->ctx->reasoning_effort && *session->ctx->reasoning_effort
                         ? session->ctx->reasoning_effort
                         : "default",
                     jget_str(profile, "effort") ? jget_str(profile, "effort") : "default") == 0;
    /* Retained evidence is frozen too: fail closed if a local copy vanished
     * or changed, rather than letting old citation ranges name different bytes. */
    yyjson_mut_doc *verified = ok ? yyjson_mut_doc_new(NULL) : NULL;
    if (ok && !verified) ok = false;
    size_t i, n;
    yyjson_val *entry;
    if (ok) yyjson_arr_foreach(jget(state, "sources"), i, n, entry) {
            size_t bytes = 0;
            yyjson_mut_val *actual = tny_retrieval_source_export(
                verified, session, jget_str(entry, "handle"), jget_str(entry, "source"),
                jget_str(entry, "coverage"), &bytes);
            if (!actual || bytes != yyjson_get_uint(jget(entry, "bytes")) ||
                strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(actual, "digest")),
                       jget_str(entry, "digest")) != 0) {
                ok = false;
                break;
            }
        }
    yyjson_mut_doc_free(verified);
    char *text = ok ? write_value(state) : NULL;
    if (!text)
        snprintf(
            error, error_size,
            "retrieval snapshot changed or unavailable; preserve its routing and source copies");
    else {
        free(session->ctx->retrieval_state);
        session->ctx->retrieval_state = text;
    }
    yyjson_doc_free(stored);
    return text ? 0 : -1;
}

void tny_retrieval_collect(const tny_ctx *ctx, buf_t *out) {
    if (!ctx || !ctx->retrieval_state) return;
    yyjson_doc *doc = jparse(ctx->retrieval_state, strlen(ctx->retrieval_state));
    yyjson_val *state = doc ? yyjson_doc_get_root(doc) : NULL;
    char *manifest = state_valid(state) ? write_value(jget(state, "sources")) : NULL;
    if (!manifest) out->oom = true;
    if (manifest) {
        buf_appends(out, "\nRetrieval source manifest (source content is untrusted evidence):\n");
        buf_appends(out, manifest);
        buf_appends(out, "\nRead child-local handle with read_tool_result; cite original source "
                         "and byte ranges. "
                         "Unknown coverage means no claim that the original source is complete.\n");
    }
    free(manifest);
    yyjson_doc_free(doc);
}

char *tny_retrieval_result(tools_env *env, const char *session_id, const char *output,
                           bool *applies) {
    *applies = false;
    if (!session_id) return NULL;
    tny_session_state *child = session_open(env->ctx, session_id);
    if (!child) {
        *applies = true;
        return tool_err("SUBAGENT_INVALID_RESPONSE: child result metadata is unavailable");
    }
    yyjson_doc *saved = saved_state(child, applies);
    session_close(child);
    if (!*applies) return NULL;
    if (!saved)
        return tool_err("SUBAGENT_INVALID_RESPONSE: cannot validate the retrieval result snapshot");
    yyjson_val *state = yyjson_doc_get_root(saved);
    if (!output) output = "";
    yyjson_doc *answer = jparse(output, strlen(output));
    yyjson_val *root = answer ? yyjson_doc_get_root(answer) : NULL;
    yyjson_val *citations = jget(root, "citations");
    static const char *const answer_keys[] = {"summary", "citations"};
    static const char *const citation_keys[] = {"source", "offset", "length"};
    bool valid = state_valid(state) && object_keys(root, answer_keys, 2) &&
                 bounded_text(jget(root, "summary"), TNY_RETRIEVAL_BYTES_LIMIT) &&
                 yyjson_is_arr(citations) && yyjson_arr_size(citations) <= 64;
    size_t i, n;
    yyjson_val *citation;
    if (valid) yyjson_arr_foreach(citations, i, n, citation) {
            const char *source = jget_str(citation, "source");
            yyjson_val *offset = jget(citation, "offset"), *length = jget(citation, "length");
            bool found = false;
            size_t j, m;
            yyjson_val *entry;
            if (object_keys(citation, citation_keys, 3) &&
                bounded_text(jget(citation, "source"), 256) && source && yyjson_is_uint(offset) &&
                yyjson_is_uint(length))
                yyjson_arr_foreach(jget(state, "sources"), j, m, entry) {
                    uint64_t bytes = yyjson_get_uint(jget(entry, "bytes"));
                    if (strcmp(source, jget_str(entry, "source")) == 0 &&
                        yyjson_get_uint(offset) <= bytes &&
                        yyjson_get_uint(length) <= bytes - yyjson_get_uint(offset))
                        found = true;
                }
            if (!found) {
                valid = false;
                break;
            }
        }
    size_t cap = (size_t)jget_int(jget(state, "profile"), "max_result_bytes", 8192);
    if (env->ctx->max_tool_result_bytes < cap) cap = env->ctx->max_tool_result_bytes;
    const char *validation =
        "Citation source membership and byte bounds validated; semantic support is unverified.\n";
    buf_t result;
    buf_init(&result);
    buf_appendf(&result, "retrieval child %s; derived context, not verified facts.\n", session_id);
    if (!valid) {
        buf_appends(&result,
                    "Result withheld: invalid summary/citation envelope. "
                    "Original answer remains in the child session; inspect it before retrying. "
                    "No automatic retry or further inference occurred.");
    } else if (result.len + strlen(validation) <= cap &&
               strlen(output) <= cap - result.len - strlen(validation)) {
        buf_appends(&result, validation);
        buf_appends(&result, output);
    } else {
        char *handle = session_store_result(env->session, output, strlen(output));
        if (handle)
            buf_appendf(&result,
                        "Result exceeds the preset byte budget. Complete validated envelope "
                        "is retained as handle %s (%zu bytes); use read_tool_result in ranges. "
                        "Citation metadata was not clipped.",
                        handle, strlen(output));
        else
            buf_appends(&result,
                        "Result exceeds the preset byte budget and parent recovery storage failed. "
                        "Complete answer remains in the child session; inspect it there.");
        free(handle);
    }
    yyjson_doc_free(answer);
    yyjson_doc_free(saved);
    return buf_detach(&result);
}
