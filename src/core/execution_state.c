#include "core/execution_state.h"
#include "core/execution_protocol.h"
#include "core/image.h"
#include "util/image_io.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define STATE_IMAGE_BYTES  (4u * 1024u * 1024u)
#define STATE_RESULT_BYTES TNY_EXEC_FRAME_MAX
#define STATE_ENTRIES      4096u

static bool add(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, yyjson_mut_val *v) {
    yyjson_mut_val *k = yyjson_mut_strcpy(d, key);
    return k && v && yyjson_mut_obj_add(obj, k, v);
}
static bool add_optional(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const char *text) {
    return add(d, o, key, text ? yyjson_mut_strcpy(d, text) : yyjson_mut_null(d));
}
static bool add_bytes(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const void *bytes,
                      size_t len) {
    buf_t b;
    buf_init(&b);
    b64_encode(bytes, len, &b);
    bool ok = !b.oom && yyjson_mut_obj_add_strcpy(d, o, key, b.data ? b.data : "");
    buf_free(&b);
    return ok;
}
static bool array_prefix(yyjson_val *before, yyjson_val *after) {
    if (!yyjson_is_arr(after) || (before && !yyjson_is_arr(before))) return false;
    size_t count = yyjson_arr_size(before);
    if (count > yyjson_arr_size(after)) return false;
    yyjson_arr_iter previous = yyjson_arr_iter_with(before), current = yyjson_arr_iter_with(after);
    yyjson_val *item;
    while ((item = yyjson_arr_iter_next(&previous)))
        if (!yyjson_equals(item, yyjson_arr_iter_next(&current))) return false;
    return true;
}
static yyjson_mut_val *encode_session(yyjson_mut_doc *d, tools_env *env, yyjson_val *baseline,
                                      bool initial) {
    tny_session_state *s = env->session;
    if (!s) return yyjson_mut_null(d);
    yyjson_doc *snapshot = yyjson_mut_doc_imut_copy(s->doc, jallocator());
    if (!snapshot) return NULL;
    yyjson_val *root = yyjson_doc_get_root(snapshot);
    yyjson_mut_val *out = yyjson_mut_obj(d);
    bool ok = out && yyjson_is_obj(root);
    if (initial) {
        ok = ok && add_optional(d, out, "id", s->id) && add_optional(d, out, "dir", s->dir) &&
             add(d, out, "doc", yyjson_val_mut_copy(d, root)) &&
             add_optional(d, out, "task_body", s->task_body) &&
             add_optional(d, out, "extension_start_reason", s->extension_start_reason) &&
             add_optional(d, out, "extension_previous_session_id",
                          s->extension_previous_session_id) &&
             yyjson_mut_obj_add_uint(d, out, "extension_event_sequence",
                                     s->extension_event_sequence) &&
             yyjson_mut_obj_add_uint(d, out, "extension_agent_sequence",
                                     s->extension_agent_sequence) &&
             yyjson_mut_obj_add_bool(d, out, "extension_session_started",
                                     s->extension_session_started) &&
             yyjson_mut_obj_add_bool(d, out, "persisted", s->persisted);
    } else {
        yyjson_mut_val *set = yyjson_mut_obj(d), *remove = yyjson_mut_arr(d),
                       *append = yyjson_mut_obj(d);
        ok = ok && yyjson_is_obj(baseline) && add(d, out, "set", set) &&
             add(d, out, "remove", remove) && add(d, out, "append", append);
        if (ok) {
            size_t i, n;
            yyjson_val *k, *v;
            yyjson_obj_foreach(root, i, n, k, v) {
                const char *key = yyjson_get_str(k);
                yyjson_val *previous = yyjson_obj_get(baseline, key);
                if (yyjson_equals(v, previous)) continue;
                if (array_prefix(previous, v)) {
                    yyjson_mut_val *suffix = yyjson_mut_arr(d);
                    ok = ok && suffix;
                    size_t j, count;
                    yyjson_val *item;
                    yyjson_arr_foreach(v, j, count, item) {
                        if (!ok || j < yyjson_arr_size(previous)) continue;
                        yyjson_mut_val *copy = yyjson_val_mut_copy(d, item);
                        ok = copy && yyjson_mut_arr_append(suffix, copy);
                    }
                    ok = ok && add(d, append, key, suffix);
                } else ok = ok && add(d, set, key, yyjson_val_mut_copy(d, v));
            }
            yyjson_obj_foreach(baseline, i, n, k, v) {
                (void)v;
                const char *key = yyjson_get_str(k);
                if (!yyjson_obj_get(root, key))
                    ok = ok && yyjson_mut_arr_add_strcpy(d, remove, key);
            }
        }
    }
    yyjson_doc_free(snapshot);
    return ok ? out : NULL;
}
yyjson_mut_val *tny_execution_state_encode(yyjson_mut_doc *d, tools_env *env, yyjson_val *baseline,
                                           bool initial) {
    if (!d || !env || env->n_pending_images < 0 || env->n_pending_images > 8) return NULL;
    yyjson_mut_val *out = yyjson_mut_obj(d), *grants = yyjson_mut_arr(d),
                   *results = yyjson_mut_arr(d), *images = yyjson_mut_arr(d),
                   *fact = yyjson_mut_obj(d);
    bool ok = out && add(d, out, "session", encode_session(d, env, baseline, initial)) &&
              add(d, out, "grants", grants) && add(d, out, "mem_results", results) &&
              add(d, out, "images", images) && add(d, out, "learning", fact) &&
              yyjson_mut_obj_add_uint(d, out, "pending_count", (unsigned)env->n_pending_images) &&
              yyjson_mut_obj_add_bool(d, out, "perm_blocked", env->perm_blocked) &&
              yyjson_mut_obj_add_bool(d, fact, "valid", env->learning_fact.valid) &&
              yyjson_mut_obj_add_bool(d, fact, "ok", env->learning_fact.ok) &&
              yyjson_mut_obj_add_uint(d, fact, "event", (unsigned)env->learning_fact.event) &&
              yyjson_mut_obj_add_uint(d, fact, "scope", env->learning_fact.scope) &&
              yyjson_mut_obj_add_uint(d, fact, "intent", env->learning_fact.intent);
    if (env->perm) {
        if (env->perm->n_grants < 0 || (unsigned)env->perm->n_grants > STATE_ENTRIES) return NULL;
        for (int i = 0; ok && i < env->perm->n_grants; ++i)
            ok = yyjson_mut_arr_add_strcpy(d, grants, env->perm->grants[i]);
    }
    if (env->session) {
        tny_session_state *s = env->session;
        if (s->n_mem_results < 0 || (unsigned)s->n_mem_results > STATE_ENTRIES) return NULL;
        size_t total = 0;
        for (int i = 0; ok && i < s->n_mem_results; ++i) {
            session_mem_result *r = &s->mem_results[i];
            if (r->len > STATE_RESULT_BYTES - total) return NULL;
            total += r->len;
            yyjson_mut_val *item = yyjson_mut_obj(d);
            ok = item && yyjson_mut_obj_add_strcpy(d, item, "handle", r->handle) &&
                 yyjson_mut_obj_add_uint(d, item, "len", r->len) &&
                 add_bytes(d, item, "data", r->data, r->len) &&
                 yyjson_mut_arr_append(results, item);
        }
    }
    size_t total = 0;
    for (int i = 0; ok && !initial && i < env->n_pending_images; ++i) {
        tools_pending_capture *cap = &env->pending_capture[i];
        if (!cap->data || !cap->len || cap->len > STATE_IMAGE_BYTES - total || !cap->mime ||
            !env->pending_images[i])
            return NULL;
        total += cap->len;
        yyjson_mut_val *item = yyjson_mut_obj(d);
        ok = item && yyjson_mut_obj_add_strcpy(d, item, "path", env->pending_images[i]) &&
             yyjson_mut_obj_add_strcpy(d, item, "mime", cap->mime) &&
             yyjson_mut_obj_add_strcpy(d, item, "sha256", cap->sha256) &&
             yyjson_mut_obj_add_uint(d, item, "origin", (unsigned)cap->origin) &&
             yyjson_mut_obj_add_uint(d, item, "len", cap->len) &&
             add_bytes(d, item, "data", cap->data, cap->len) && yyjson_mut_arr_append(images, item);
    }
    return ok ? out : NULL;
}

/* Canonical re-encoding rejects ignored characters, padding aliases and
 * truncation accepted by the general-purpose base64 decoder. */
static char *read_bytes(yyjson_val *item, size_t max, size_t *len_out) {
    yyjson_val *size = jget(item, "len");
    const char *encoded = jget_str(item, "data");
    if (!yyjson_is_uint(size) || !encoded || yyjson_get_uint(size) > max) return NULL;
    size_t len = (size_t)yyjson_get_uint(size);
    if (strlen(encoded) != ((len + 2) / 3) * 4) return NULL;
    char *bytes = malloc(len + 1);
    if (!bytes) return NULL;
    size_t got = b64_decode(encoded, (uint8_t *)bytes, len);
    buf_t canonical;
    buf_init(&canonical);
    b64_encode((uint8_t *)bytes, got, &canonical);
    bool ok =
        got == len && !canonical.oom && strcmp(encoded, canonical.data ? canonical.data : "") == 0;
    buf_free(&canonical);
    if (!ok) {
        free(bytes);
        return NULL;
    }
    bytes[len] = 0;
    *len_out = len;
    return bytes;
}
static bool optional_string(yyjson_val *obj, const char *key, char **out) {
    yyjson_val *v = jget(obj, key);
    if (yyjson_is_null(v)) return true;
    if (!yyjson_is_str(v)) return false;
    *out = xstrdup(yyjson_get_str(v));
    return *out != NULL;
}
static tny_session_state *restore_session(tny_ctx *ctx, yyjson_val *v) {
    const char *id = jget_str(v, "id"), *dir = jget_str(v, "dir");
    yyjson_val *doc = jget(v, "doc"), *event = jget(v, "extension_event_sequence"),
               *agent = jget(v, "extension_agent_sequence"),
               *started = jget(v, "extension_session_started"), *persisted = jget(v, "persisted");
    if (!id || !dir || !yyjson_is_obj(doc) || !yyjson_is_uint(event) || !yyjson_is_uint(agent) ||
        !yyjson_is_bool(started) || !yyjson_is_bool(persisted))
        return NULL;
    tny_session_state *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx = ctx;
    s->lock_fd = -1;
    s->execution_snapshot = true;
    s->id = xstrdup(id);
    s->dir = xstrdup(dir);
    s->doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *root = s->doc ? yyjson_val_mut_copy(s->doc, doc) : NULL;
    bool ok =
        s->id && s->dir && root && optional_string(v, "task_body", &s->task_body) &&
        optional_string(v, "extension_start_reason", &s->extension_start_reason) &&
        optional_string(v, "extension_previous_session_id", &s->extension_previous_session_id);
    if (!ok) {
        session_close(s);
        return NULL;
    }
    yyjson_mut_doc_set_root(s->doc, root);
    s->extension_event_sequence = yyjson_get_uint(event);
    s->extension_agent_sequence = yyjson_get_uint(agent);
    s->extension_session_started = yyjson_get_bool(started);
    s->persisted = yyjson_get_bool(persisted);
    return s;
}
static yyjson_mut_doc *merge_doc(tny_session_state *s, yyjson_val *delta) {
    yyjson_val *set = jget(delta, "set"), *remove = jget(delta, "remove"),
               *append = jget(delta, "append");
    if (!s || yyjson_obj_size(delta) != 3 || !yyjson_is_obj(set) || !yyjson_is_arr(remove) ||
        !yyjson_is_obj(append))
        return NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_mut_copy(s->doc, jallocator());
    if (!d) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    if (!yyjson_mut_is_obj(root)) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(set, i, n, key, value) {
        yyjson_mut_val *k = yyjson_val_mut_copy(d, key), *v = yyjson_val_mut_copy(d, value);
        if (!k || !v || !yyjson_mut_obj_put(root, k, v)) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
    }
    yyjson_obj_foreach(append, i, n, key, value) {
        const char *name = yyjson_get_str(key);
        if (!yyjson_is_arr(value) || yyjson_obj_get(set, name)) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
        yyjson_mut_val *array = yyjson_mut_obj_get(root, name);
        if (!array) {
            array = yyjson_mut_arr(d);
            if (!add(d, root, name, array)) {
                yyjson_mut_doc_free(d);
                return NULL;
            }
        }
        if (!yyjson_mut_is_arr(array)) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
        size_t j, count;
        yyjson_val *item;
        yyjson_arr_foreach(value, j, count, item) {
            yyjson_mut_val *copy = yyjson_val_mut_copy(d, item);
            if (!copy || !yyjson_mut_arr_append(array, copy)) {
                yyjson_mut_doc_free(d);
                return NULL;
            }
        }
    }
    yyjson_arr_foreach(remove, i, n, key) {
        if (!yyjson_is_str(key) || yyjson_obj_get(set, yyjson_get_str(key)) ||
            yyjson_obj_get(append, yyjson_get_str(key))) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
        yyjson_mut_obj_remove_key(root, yyjson_get_str(key));
    }
    size_t encoded_len = 0;
    char *encoded = yyjson_mut_write_opts(d, 0, jallocator(), &encoded_len, NULL);
    bool bounded = encoded && encoded_len <= TNY_EXEC_FRAME_MAX;
    free(encoded);
    if (!bounded) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}
static void free_results(session_mem_result *r, int count) {
    for (int i = 0; i < count; ++i) {
        free(r[i].handle);
        free(r[i].data);
    }
    free(r);
}
bool tny_execution_state_apply(tools_env *env, yyjson_val *state, bool initial) {
    if (!env || !env->ctx || !yyjson_is_obj(state) || !tny_exec_json_valid(state) ||
        (initial && env->session))
        return false;
    yyjson_val *session = jget(state, "session"), *grants = jget(state, "grants"),
               *results = jget(state, "mem_results"), *images = jget(state, "images"),
               *fact = jget(state, "learning"), *blocked = jget(state, "perm_blocked"),
               *pending = jget(state, "pending_count");
    if ((!yyjson_is_obj(session) && !yyjson_is_null(session)) || !yyjson_is_arr(grants) ||
        !yyjson_is_arr(results) || !yyjson_is_arr(images) || !yyjson_is_obj(fact) ||
        !yyjson_is_bool(blocked) || !yyjson_is_uint(pending) || yyjson_get_uint(pending) > 8 ||
        yyjson_arr_size(grants) > STATE_ENTRIES || yyjson_arr_size(results) > STATE_ENTRIES ||
        yyjson_arr_size(images) > 8 || (initial && yyjson_arr_size(images)))
        return false;
    yyjson_val *valid = jget(fact, "valid"), *ok = jget(fact, "ok"), *event = jget(fact, "event"),
               *scope = jget(fact, "scope"), *intent = jget(fact, "intent");
    if (!yyjson_is_bool(valid) || !yyjson_is_bool(ok) || !yyjson_is_uint(event) ||
        yyjson_get_uint(event) > TNY_LEARN_OTHER || !yyjson_is_uint(scope) ||
        !yyjson_is_uint(intent))
        return false;
    tools_learning_fact learning = {.valid = yyjson_get_bool(valid),
                                    .ok = yyjson_get_bool(ok),
                                    .event = (tny_learning_event)yyjson_get_uint(event),
                                    .scope = yyjson_get_uint(scope),
                                    .intent = yyjson_get_uint(intent)};
    tny_session_state *fresh = NULL;
    yyjson_mut_doc *merged = NULL;
    char **new_grants = NULL;
    int n_grants = 0, n_results = 0;
    session_mem_result *new_results = NULL;
    tools_env image_stage = {0};
    bool success = false;
    if (yyjson_is_obj(session)) {
        if (initial) {
            fresh = restore_session(env->ctx, session);
            if (!fresh) goto done;
        } else {
            merged = merge_doc(env->session, session);
            if (!merged) goto done;
        }
    } else if (!initial && env->session) goto done;
    tny_session_state *target = initial ? fresh : env->session;
    if (yyjson_arr_size(results) && !target) goto done;
    size_t existing_grants = env->perm && env->perm->n_grants > 0 ? (size_t)env->perm->n_grants : 0;
    if (existing_grants > STATE_ENTRIES || (!env->perm && yyjson_arr_size(grants))) goto done;
    new_grants = calloc(existing_grants + yyjson_arr_size(grants) + 1, sizeof(*new_grants));
    if (!new_grants) goto done;
    for (size_t i = 0; i < existing_grants; ++i) {
        new_grants[n_grants] = xstrdup(env->perm->grants[i]);
        if (!new_grants[n_grants]) goto done;
        ++n_grants;
    }
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(grants, i, n, item) {
        const char *grant = yyjson_get_str(item);
        if (!grant || !*grant) goto done;
        bool found = false;
        for (int j = 0; j < n_grants; ++j)
            if (!strcmp(grant, new_grants[j])) found = true;
        if (found) continue;
        if ((unsigned)n_grants >= STATE_ENTRIES) goto done;
        new_grants[n_grants] = xstrdup(grant);
        if (!new_grants[n_grants]) goto done;
        ++n_grants;
    }
    size_t existing_results =
        target && target->n_mem_results > 0 ? (size_t)target->n_mem_results : 0;
    if (existing_results > STATE_ENTRIES) goto done;
    new_results = calloc(existing_results + yyjson_arr_size(results) + 1, sizeof(*new_results));
    if (!new_results) goto done;
    size_t result_bytes = 0;
    for (size_t j = 0; j < existing_results; ++j) {
        session_mem_result *old = &target->mem_results[j], *r = &new_results[n_results++];
        if (old->len > STATE_RESULT_BYTES - result_bytes) goto done;
        result_bytes += old->len;
        r->handle = xstrdup(old->handle);
        r->data = malloc(old->len + 1);
        if (!r->handle || !r->data) goto done;
        memcpy(r->data, old->data, old->len);
        r->data[old->len] = 0;
        r->len = old->len;
    }
    yyjson_arr_foreach(results, i, n, item) {
        const char *handle = jget_str(item, "handle");
        if (!handle || !*handle || strlen(handle) > 256) goto done;
        size_t len = 0;
        char *data = read_bytes(item, STATE_RESULT_BYTES, &len);
        if (!data) goto done;
        int found = -1;
        for (int j = 0; j < n_results; ++j)
            if (!strcmp(handle, new_results[j].handle)) found = j;
        if (found >= 0) {
            bool equal =
                len == new_results[found].len && !memcmp(data, new_results[found].data, len);
            free(data);
            if (!equal) goto done;
            continue;
        }
        if (len > STATE_RESULT_BYTES - result_bytes) {
            free(data);
            goto done;
        }
        if ((unsigned)n_results >= STATE_ENTRIES) {
            free(data);
            goto done;
        }
        result_bytes += len;
        session_mem_result *r = &new_results[n_results++];
        r->data = data;
        r->len = len;
        r->handle = xstrdup(handle);
        if (!r->handle) goto done;
    }
    if (env->n_pending_images < 0 || env->n_pending_images > 8 ||
        yyjson_arr_size(images) > (size_t)(8 - env->n_pending_images))
        goto done;
    size_t image_bytes = 0;
    for (int j = 0; j < env->n_pending_images; ++j) {
        if (env->pending_capture[j].len > STATE_IMAGE_BYTES - image_bytes) goto done;
        image_bytes += env->pending_capture[j].len;
    }
    yyjson_arr_foreach(images, i, n, item) {
        const char *path = jget_str(item, "path"), *mime = jget_str(item, "mime"),
                   *hash = jget_str(item, "sha256");
        yyjson_val *origin = jget(item, "origin");
        if (!path || !*path || !mime || !hash || strlen(hash) != 64 || !yyjson_is_uint(origin) ||
            yyjson_get_uint(origin) > TNY_IMAGE_QUEUE_PREVIEW)
            goto done;
        int slot = image_stage.n_pending_images++;
        tools_pending_capture *cap = &image_stage.pending_capture[slot];
        cap->data = (uint8_t *)read_bytes(item, STATE_IMAGE_BYTES - image_bytes, &cap->len);
        image_stage.pending_images[slot] = xstrdup(path);
        if (!cap->data || !cap->len || !image_stage.pending_images[slot]) goto done;
        image_bytes += cap->len;
        cap->mime = image_mime(cap->data, cap->len);
        cap->origin = (tny_image_queue_origin)yyjson_get_uint(origin);
        if (!cap->mime || strcmp(cap->mime, mime) != 0 ||
            !tny_image_io_sha256_hex(cap->data, cap->len, cap->sha256) ||
            strcmp(cap->sha256, hash) != 0)
            goto done;
    }
    bool publish_session =
        !initial && env->session && yyjson_is_obj(session) &&
        (yyjson_obj_size(jget(session, "set")) || yyjson_arr_size(jget(session, "remove")) ||
         yyjson_obj_size(jget(session, "append")));
    /* Commit memory atomically. The final owner-only durable save may fail;
     * retain committed memory and report failure so the caller cannot ACK it. */
    if (initial) {
        env->session = fresh;
        fresh = NULL;
    } else if (merged) {
        yyjson_mut_doc_free(env->session->doc);
        env->session->doc = merged;
        merged = NULL;
    }
    if (env->perm) {
        for (int j = 0; j < env->perm->n_grants; ++j) free(env->perm->grants[j]);
        free(env->perm->grants);
        env->perm->grants = new_grants;
        env->perm->n_grants = n_grants;
        new_grants = NULL;
        n_grants = 0;
    }
    if (target) {
        free_results(target->mem_results, target->n_mem_results);
        target->mem_results = new_results;
        target->n_mem_results = n_results;
        new_results = NULL;
        n_results = 0;
    }
    for (int j = 0; j < image_stage.n_pending_images; ++j) {
        int slot = env->n_pending_images++;
        env->pending_images[slot] = image_stage.pending_images[j];
        env->pending_capture[slot] = image_stage.pending_capture[j];
        image_stage.pending_images[j] = NULL;
        image_stage.pending_capture[j].data = NULL;
    }
    env->learning_fact = learning;
    env->perm_blocked = env->perm_blocked || yyjson_get_bool(blocked);
    success = !publish_session || session_save(env->session) == 0;
done:
    session_close(fresh);
    yyjson_mut_doc_free(merged);
    for (int j = 0; j < n_grants; ++j) free(new_grants[j]);
    free(new_grants);
    free_results(new_results, n_results);
    tools_discard_pending_images(&image_stage);
    return success;
}
