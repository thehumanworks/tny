#include "greatest.h"
#include "core/execution_state.h"
#include "core/image.h"
#include "util/image_io.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool set_run(tny_session_state *s, const char *run) {
    yyjson_mut_val *array = yyjson_mut_arr(s->doc);
    return array && yyjson_mut_arr_add_strcpy(s->doc, array, run) &&
           yyjson_mut_obj_put(yyjson_mut_doc_get_root(s->doc), yyjson_mut_str(s->doc, "team_runs"),
                              array);
}
static const char *first_run(tny_session_state *s) {
    return yyjson_mut_get_str(
        yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "team_runs"), 0));
}
static tny_session_state *state_session(tny_ctx *ctx) {
    tny_session_state *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx = ctx;
    s->lock_fd = -1;
    s->id = xstrdup("test-session");
    s->dir = xstrdup("/unopened-session");
    s->extension_start_reason = xstrdup("restored");
    s->extension_event_sequence = 12;
    s->extension_agent_sequence = 7;
    s->extension_session_started = true;
    s->task_body = xstrdup("private task body");
    const char *input = "{\"messages\":[],\"team_cursors\":{},\"swarm_cap\":1}";
    yyjson_doc *d = jparse(input, strlen(input));
    s->doc = d ? yyjson_doc_mut_copy(d, jallocator()) : NULL;
    yyjson_doc_free(d);
    if (!s->id || !s->dir || !s->doc || !s->task_body || !s->extension_start_reason) {
        session_close(s);
        return NULL;
    }
    return s;
}
static yyjson_doc *state_snapshot(tools_env *env, yyjson_val *baseline, bool initial) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    if (!d) return NULL;
    yyjson_mut_val *v = tny_execution_state_encode(d, env, baseline, initial);
    yyjson_doc *snapshot = NULL;
    if (v) {
        yyjson_mut_doc_set_root(d, v);
        snapshot = yyjson_mut_doc_imut_copy(d, jallocator());
    }
    yyjson_mut_doc_free(d);
    return snapshot;
}
typedef struct {
    tools_env *owner, *snapshot;
    yyjson_doc *baseline;
    const char *path;
    unsigned acknowledgments;
    bool persisted_before_ack;
} state_save_rpc;

static int owner_save_rpc(void *userdata) {
    state_save_rpc *rpc = userdata;
    yyjson_doc *delta = state_snapshot(rpc->snapshot, yyjson_doc_get_root(rpc->baseline), false);
    if (!delta) return -1;
    bool ok = tny_execution_state_apply(rpc->owner, yyjson_doc_get_root(delta), false);
    yyjson_doc_free(delta);
    if (!ok) return -1;
    yyjson_doc *disk = jparse_file(rpc->path);
    const char *marker =
        disk ? yyjson_get_str(yyjson_arr_get(jget(yyjson_doc_get_root(disk), "team_runs"), 0))
             : NULL;
    rpc->persisted_before_ack = marker && strcmp(marker, "merged-before-ack") == 0;
    yyjson_doc_free(disk);
    yyjson_doc *next = yyjson_mut_doc_imut_copy(rpc->snapshot->session->doc, jallocator());
    if (!next || !rpc->persisted_before_ack) {
        yyjson_doc_free(next);
        return -1;
    }
    yyjson_doc_free(rpc->baseline);
    rpc->baseline = next;
    ++rpc->acknowledgments;
    return 0;
}
static bool state_image(tools_env *env, const char *path) {
    static const uint8_t png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0};
    int i = env->n_pending_images++;
    tools_pending_capture *cap = &env->pending_capture[i];
    env->pending_images[i] = xstrdup(path);
    cap->data = malloc(sizeof(png));
    if (!env->pending_images[i] || !cap->data) return false;
    memcpy(cap->data, png, sizeof(png));
    cap->len = sizeof(png);
    cap->mime = image_mime(cap->data, cap->len);
    cap->origin = TNY_IMAGE_QUEUE_MANUAL;
    return cap->mime && tny_image_io_sha256_hex(cap->data, cap->len, cap->sha256);
}
static void state_cleanup(tools_env *env) {
    tools_discard_pending_images(env);
    session_close(env->session);
    perm_free(env->perm);
}
TEST execution_state_initial_and_delta_preserve_parent(void) {
    tny_ctx ctx = {.no_save = true};
    tools_env parent = {.ctx = &ctx, .perm = perm_new(&ctx)},
              child = {.ctx = &ctx, .perm = perm_new(&ctx)};
    parent.session = state_session(&ctx);
    ASSERT(parent.session && parent.perm && child.perm);
    perm_grant(parent.perm, "read_file", "/parent");
    char *h1 = session_store_result(parent.session, "a\0b", 3);
    ASSERT(h1);
    ASSERT(state_image(&parent, "/never-reopened-parent.png"));
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(initial), true));
    ASSERT(child.session);
    ASSERT_STR_EQ(parent.session->id, child.session->id);
    ASSERT_STR_EQ("private task body", child.session->task_body);
    ASSERT_EQ(12u, child.session->extension_event_sequence);
    ASSERT_EQ(-1, child.session->lock_fd);
    ASSERT_EQ(0, child.n_pending_images);
    ASSERT_EQ(1, child.session->n_mem_results);
    ASSERT_EQ(0, memcmp(child.session->mem_results[0].data, "a\0b", 3));
    yyjson_doc *baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(child.session->doc);
    ASSERT(set_run(child.session, "server-change"));
    yyjson_mut_obj_remove_key(root, "swarm_cap");
    ASSERT(yyjson_mut_obj_add_strcpy(parent.session->doc,
                                     yyjson_mut_doc_get_root(parent.session->doc), "callback",
                                     "parent-change"));
    session_add_text(parent.session, "user", "callback message");
    perm_grant(child.perm, "read_file", "/child");
    perm_grant(parent.perm, "read_file", "/callback");
    char *h2 = session_store_result(child.session, "child-result", 12);
    char *h3 = session_store_result(parent.session, "parent-result", 13);
    ASSERT(h2 && h3);
    ASSERT(state_image(&child, "/never-reopened-child.png"));
    child.perm_blocked = true;
    child.learning_fact = (tools_learning_fact){
        .valid = true, .ok = true, .event = TNY_LEARN_EDIT, .scope = 123, .intent = 456};
    yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    ASSERT(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    root = yyjson_mut_doc_get_root(parent.session->doc);
    ASSERT_STR_EQ("server-change", first_run(parent.session));
    ASSERT_STR_EQ("parent-change", yyjson_mut_get_str(yyjson_mut_obj_get(root, "callback")));
    ASSERT(yyjson_mut_obj_get(root, "swarm_cap") == NULL);
    ASSERT_EQ(1u, yyjson_mut_arr_size(yyjson_mut_obj_get(root, "messages")));
    ASSERT_EQ(3, parent.perm->n_grants);
    ASSERT_EQ(3, parent.session->n_mem_results);
    ASSERT_EQ(2, parent.n_pending_images);
    ASSERT_STR_EQ("/never-reopened-parent.png", parent.pending_images[0]);
    ASSERT_STR_EQ("/never-reopened-child.png", parent.pending_images[1]);
    ASSERT_EQ(0, memcmp(parent.pending_capture[1].data, child.pending_capture[0].data, 8));
    ASSERT(parent.perm_blocked && parent.learning_fact.valid);
    ASSERT_EQ(456u, parent.learning_fact.intent);
    ASSERT_EQ(12u, parent.session->extension_event_sequence);
    free(h1);
    free(h2);
    free(h3);
    yyjson_doc_free(initial);
    yyjson_doc_free(baseline);
    yyjson_doc_free(delta);
    state_cleanup(&parent);
    state_cleanup(&child);
    PASS();
}
TEST execution_state_failed_apply_is_atomic(void) {
    tny_ctx ctx = {.no_save = true};
    tools_env parent = {.ctx = &ctx, .perm = perm_new(&ctx)},
              child = {.ctx = &ctx, .perm = perm_new(&ctx)};
    parent.session = state_session(&ctx);
    ASSERT(parent.session && parent.perm && child.perm);
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(initial), true));
    yyjson_doc *baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    ASSERT(set_run(child.session, "changed"));
    perm_grant(child.perm, "read_file", "/new-grant");
    ASSERT(state_image(&child, "/never-reopened.png"));
    child.pending_capture[0].sha256[0] = child.pending_capture[0].sha256[0] == 'a' ? 'b' : 'a';
    yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    yyjson_mut_doc *original_doc = parent.session->doc;
    ASSERT_FALSE(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    ASSERT(parent.session->doc == original_doc);
    ASSERT(yyjson_mut_obj_get(yyjson_mut_doc_get_root(original_doc), "team_runs") == NULL);
    ASSERT_EQ(0, parent.perm->n_grants);
    ASSERT_EQ(0, parent.n_pending_images);
    yyjson_doc_free(initial);
    yyjson_doc_free(baseline);
    yyjson_doc_free(delta);
    state_cleanup(&parent);
    state_cleanup(&child);
    PASS();
}
TEST execution_state_null_session(void) {
    tny_ctx ctx = {0};
    tools_env parent = {.ctx = &ctx}, child = {.ctx = &ctx};
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(initial), true));
    ASSERT(child.session == NULL);
    yyjson_doc_free(initial);
    PASS();
}
TEST execution_snapshot_defers_save_to_merged_owner(void) {
    char directory[] = "/tmp/tny-execution-state-XXXXXX";
    ASSERT(mkdtemp(directory));
    tny_ctx ctx = {0};
    tools_env parent = {.ctx = &ctx, .perm = perm_new(&ctx)};
    tools_env child = {.ctx = &ctx, .perm = perm_new(&ctx)};
    parent.session = state_session(&ctx);
    ASSERT(parent.session && parent.perm && child.perm);
    free(parent.session->dir);
    parent.session->dir = xstrdup(directory);
    ASSERT(parent.session->dir);
    ASSERT_EQ(0, session_save(parent.session));
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(initial), true));
    ASSERT(child.session->execution_snapshot);
    ASSERT_FALSE(parent.session->execution_snapshot);
    yyjson_doc *baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    ASSERT(yyjson_mut_obj_add_strcpy(parent.session->doc,
                                     yyjson_mut_doc_get_root(parent.session->doc), "owner_hook",
                                     "preserve-on-disk"));
    ASSERT_EQ(0, session_save(parent.session));
    ASSERT(set_run(child.session, "merged-before-ack"));
    ASSERT_EQ(-1, session_save(child.session));
    char *path = path_join(directory, "session.json");
    ASSERT(path);
    yyjson_doc *disk = jparse_file(path);
    ASSERT(disk);
    ASSERT_STR_EQ("preserve-on-disk", jget_str(yyjson_doc_get_root(disk), "owner_hook"));
    ASSERT(jget(yyjson_doc_get_root(disk), "team_runs") == NULL);
    yyjson_doc_free(disk);
    state_save_rpc rpc = {.owner = &parent, .snapshot = &child, .baseline = baseline, .path = path};
    child.session->execution_save = owner_save_rpc;
    child.session->execution_save_ud = &rpc;
    ASSERT_EQ(0, session_save(child.session));
    ASSERT_EQ(1u, rpc.acknowledgments);
    ASSERT(rpc.persisted_before_ack);
    baseline = rpc.baseline;
    disk = jparse_file(path);
    ASSERT(disk);
    ASSERT_STR_EQ("preserve-on-disk", jget_str(yyjson_doc_get_root(disk), "owner_hook"));
    ASSERT_STR_EQ("merged-before-ack",
                  yyjson_get_str(yyjson_arr_get(jget(yyjson_doc_get_root(disk), "team_runs"), 0)));
    yyjson_doc_free(disk);
    yyjson_doc_free(baseline);
    baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    /* A regular file cannot contain session.json: unchanged deltas must not
     * attempt publication, while changed-delta failures retain merged memory. */
    char *blocked = path_join(directory, "not-a-directory");
    ASSERT(blocked);
    ASSERT_EQ(0, file_write_atomic(blocked, "sentinel", 8));
    free(parent.session->dir);
    parent.session->dir = xstrdup(blocked);
    ASSERT(parent.session->dir);
    yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    ASSERT(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    yyjson_doc_free(delta);
    ASSERT(yyjson_mut_obj_put(yyjson_mut_doc_get_root(child.session->doc),
                              yyjson_mut_str(child.session->doc, "swarm_cap"),
                              yyjson_mut_int(child.session->doc, 2)));
    delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    ASSERT_FALSE(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    ASSERT_EQ(2, yyjson_mut_get_int(yyjson_mut_obj_get(yyjson_mut_doc_get_root(parent.session->doc),
                                                       "swarm_cap")));
    yyjson_doc_free(delta);
    yyjson_doc_free(baseline);
    yyjson_doc_free(initial);
    state_cleanup(&parent);
    state_cleanup(&child);
    ASSERT_EQ(0, unlink(blocked));
    ASSERT_EQ(0, unlink(path));
    ASSERT_EQ(0, rmdir(directory));
    free(blocked);
    free(path);
    PASS();
}
static bool append_state_string(tny_session_state *session, const char *key, const char *text) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(session->doc);
    yyjson_mut_val *array = yyjson_mut_obj_get(root, key);
    if (!array) {
        array = yyjson_mut_arr(session->doc);
        if (!array || !yyjson_mut_obj_add_val(session->doc, root, key, array)) return false;
    }
    return yyjson_mut_arr_add_strcpy(session->doc, array, text);
}
TEST execution_state_appends_preserve_both_writers_exactly_once(void) {
    tny_ctx ctx = {.no_save = true};
    tools_env parent = {.ctx = &ctx}, child = {.ctx = &ctx};
    parent.session = state_session(&ctx);
    ASSERT(parent.session);
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(initial), true));
    yyjson_doc *baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    const char *fields[] = {"acp_pending_context", "team_receipts"};
    for (int round = 0; round < 2; ++round) {
        session_add_text(parent.session, "user", round ? "owner-two" : "owner-one");
        session_add_text(child.session, "user", round ? "server-two" : "server-one");
        for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
            ASSERT(
                append_state_string(parent.session, fields[i], round ? "owner-two" : "owner-one"));
            ASSERT(
                append_state_string(child.session, fields[i], round ? "server-two" : "server-one"));
        }
        yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
        ASSERT(delta);
        yyjson_val *changes = jget(yyjson_doc_get_root(delta), "session");
        ASSERT_EQ(0u, yyjson_obj_size(jget(changes, "set")));
        ASSERT_EQ(3u, yyjson_obj_size(jget(changes, "append")));
        ASSERT(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
        yyjson_doc_free(delta);
        yyjson_doc_free(baseline);
        baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
        ASSERT(baseline);
    }
    const char *expected[] = {"owner-one", "server-one", "owner-two", "server-two"};
    yyjson_mut_val *root = yyjson_mut_doc_get_root(parent.session->doc);
    yyjson_mut_val *messages = yyjson_mut_obj_get(root, "messages");
    ASSERT_EQ(4u, yyjson_mut_arr_size(messages));
    for (size_t i = 0; i < 4; ++i)
        ASSERT_STR_EQ(expected[i], yyjson_mut_get_str(yyjson_mut_obj_get(
                                       yyjson_mut_arr_get(messages, i), "content")));
    for (size_t field = 0; field < sizeof(fields) / sizeof(*fields); ++field) {
        yyjson_mut_val *array = yyjson_mut_obj_get(root, fields[field]);
        ASSERT_EQ(4u, yyjson_mut_arr_size(array));
        for (size_t i = 0; i < 4; ++i)
            ASSERT_STR_EQ(expected[i], yyjson_mut_get_str(yyjson_mut_arr_get(array, i)));
    }
    yyjson_doc *empty = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(empty);
    ASSERT(tny_execution_state_apply(&parent, yyjson_doc_get_root(empty), false));
    ASSERT_EQ(4u, yyjson_mut_arr_size(session_messages(parent.session)));
    yyjson_doc_free(empty);
    yyjson_doc_free(initial);
    yyjson_doc_free(baseline);
    state_cleanup(&parent);
    state_cleanup(&child);
    PASS();
}
TEST execution_snapshot_omits_history_and_preserves_large_owner(void) {
    char directory[] = "/tmp/tny-large-execution-state-XXXXXX";
    ASSERT(mkdtemp(directory));
    tny_ctx ctx = {0};
    tools_env parent = {.ctx = &ctx}, child = {.ctx = &ctx};
    parent.session = state_session(&ctx);
    ASSERT(parent.session);
    free(parent.session->dir);
    parent.session->dir = xstrdup(directory);
    ASSERT(parent.session->dir);
    yyjson_doc *small = state_snapshot(&parent, NULL, true);
    ASSERT(small);
    char *text = malloc(1024u * 1024u + 1);
    ASSERT(text);
    memset(text, 'x', 1024u * 1024u);
    text[1024u * 1024u] = 0;
    for (int i = 0; i < 12; ++i) session_add_text(parent.session, "user", text);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(parent.session->doc);
    ASSERT(yyjson_mut_obj_add_strcpy(parent.session->doc, root, "extension_audit", text));
    ASSERT(yyjson_mut_obj_add_strcpy(parent.session->doc, root, "skill_injections", text));
    free(text);
    yyjson_doc *large = state_snapshot(&parent, NULL, true);
    ASSERT(large);
    ASSERT(yyjson_equals(yyjson_doc_get_root(small), yyjson_doc_get_root(large)));
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(large), true));
    ASSERT_EQ(0u, yyjson_mut_arr_size(session_messages(child.session)));
    yyjson_doc *baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    yyjson_mut_doc *original = parent.session->doc;
    ASSERT(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    ASSERT(parent.session->doc == original); /* No history copy on ordinary reads. */
    yyjson_doc_free(delta);
    session_add_text(child.session, "user", "new runtime context");
    delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    ASSERT(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    ASSERT_EQ(13u, yyjson_mut_arr_size(session_messages(parent.session)));
    for (size_t i = 0; i < 12; ++i) {
        yyjson_mut_val *message = yyjson_mut_arr_get(session_messages(parent.session), i);
        ASSERT_EQ(1024u * 1024u, yyjson_mut_get_len(yyjson_mut_obj_get(message, "content")));
    }
    ASSERT_STR_EQ("new runtime context",
                  yyjson_mut_get_str(yyjson_mut_obj_get(
                      yyjson_mut_arr_get(session_messages(parent.session), 12), "content")));
    ASSERT(yyjson_mut_obj_get(yyjson_mut_doc_get_root(parent.session->doc), "extension_audit"));
    char *path = path_join(directory, "session.json");
    ASSERT(path);
    yyjson_doc *saved = jparse_file(path);
    ASSERT(saved);
    ASSERT_EQ(13u, yyjson_arr_size(jget(yyjson_doc_get_root(saved), "messages")));
    yyjson_doc_free(saved);
    ASSERT_EQ(0, unlink(path));
    ASSERT_EQ(0, rmdir(directory));
    free(path);
    yyjson_doc_free(delta);
    yyjson_doc_free(baseline);
    yyjson_doc_free(large);
    yyjson_doc_free(small);
    state_cleanup(&child);
    state_cleanup(&parent);
    PASS();
}
TEST execution_snapshot_rejects_unknown_or_historical_projection(void) {
    tny_ctx ctx = {.no_save = true};
    tools_env parent = {.ctx = &ctx};
    parent.session = state_session(&ctx);
    ASSERT(parent.session);
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    const char *versions[] = {"null", "0", "2", "1.5"};
    for (size_t i = 0; i < 7; ++i) {
        yyjson_mut_doc *bad = yyjson_doc_mut_copy(initial, jallocator());
        ASSERT(bad);
        yyjson_mut_val *session = yyjson_mut_obj_get(yyjson_mut_doc_get_root(bad), "session");
        yyjson_mut_val *doc = yyjson_mut_obj_get(session, "doc");
        if (i < 4) {
            yyjson_doc *version = jparse(versions[i], strlen(versions[i]));
            ASSERT(version);
            ASSERT(yyjson_mut_obj_put(session, yyjson_mut_str(bad, "projection"),
                                      yyjson_val_mut_copy(bad, yyjson_doc_get_root(version))));
            yyjson_doc_free(version);
        } else if (i == 4) yyjson_mut_obj_remove_key(doc, "messages");
        else if (i == 5)
            ASSERT(yyjson_mut_arr_add_strcpy(bad, yyjson_mut_obj_get(doc, "messages"), "history"));
        else ASSERT(yyjson_mut_obj_add_bool(bad, doc, "extension_audit", true));
        yyjson_doc *wire = yyjson_mut_doc_imut_copy(bad, jallocator());
        ASSERT(wire);
        tools_env child = {.ctx = &ctx, .perm = perm_new(&ctx)};
        ASSERT(child.perm);
        perm_grant(child.perm, "read_file", "/keep");
        ASSERT_FALSE(tny_execution_state_apply(&child, yyjson_doc_get_root(wire), true));
        ASSERT(child.session == NULL);
        ASSERT_EQ(1, child.perm->n_grants);
        state_cleanup(&child);
        yyjson_doc_free(wire);
        yyjson_mut_doc_free(bad);
    }
    yyjson_doc_free(initial);
    state_cleanup(&parent);
    PASS();
}
TEST execution_snapshot_refuses_owner_history_mutation(void) {
    tny_ctx ctx = {.no_save = true};
    tools_env parent = {.ctx = &ctx}, child = {.ctx = &ctx};
    parent.session = state_session(&ctx);
    ASSERT(parent.session);
    session_add_text(parent.session, "user", "retain me");
    yyjson_doc *initial = state_snapshot(&parent, NULL, true);
    ASSERT(initial);
    ASSERT(tny_execution_state_apply(&child, yyjson_doc_get_root(initial), true));
    yyjson_doc *baseline = yyjson_mut_doc_imut_copy(child.session->doc, jallocator());
    ASSERT(baseline);
    yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    const char *changes[] = {"{\"set\":{\"messages\":[]},\"append\":{},\"remove\":[]}",
                             "{\"set\":{},\"append\":{},\"remove\":[\"messages\"]}",
                             "{\"set\":{\"extension_audit\":[]},\"append\":{},\"remove\":[]}",
                             "{\"set\":{},\"append\":{\"extension_audit\":[]},\"remove\":[]}",
                             "{\"set\":{},\"append\":{},\"remove\":[\"extension_audit\"]}"};
    for (size_t i = 0; i < sizeof(changes) / sizeof(*changes); ++i) {
        yyjson_mut_doc *bad = yyjson_doc_mut_copy(delta, jallocator());
        yyjson_doc *change = jparse(changes[i], strlen(changes[i]));
        ASSERT(bad && change);
        ASSERT(yyjson_mut_obj_put(yyjson_mut_doc_get_root(bad), yyjson_mut_str(bad, "session"),
                                  yyjson_val_mut_copy(bad, yyjson_doc_get_root(change))));
        yyjson_doc *wire = yyjson_mut_doc_imut_copy(bad, jallocator());
        ASSERT(wire);
        yyjson_mut_doc *original = parent.session->doc;
        ASSERT_FALSE(tny_execution_state_apply(&parent, yyjson_doc_get_root(wire), false));
        ASSERT(parent.session->doc == original);
        yyjson_doc_free(wire);
        yyjson_doc_free(change);
        yyjson_mut_doc_free(bad);
    }
    yyjson_doc_free(delta);
    yyjson_doc_free(baseline);
    yyjson_doc_free(initial);
    state_cleanup(&child);
    state_cleanup(&parent);
    PASS();
}
SUITE(execution_state_suite) {
    RUN_TEST(execution_snapshot_rejects_unknown_or_historical_projection);
    RUN_TEST(execution_snapshot_omits_history_and_preserves_large_owner);
    RUN_TEST(execution_snapshot_refuses_owner_history_mutation);
    RUN_TEST(execution_state_initial_and_delta_preserve_parent);
    RUN_TEST(execution_state_failed_apply_is_atomic);
    RUN_TEST(execution_state_null_session);
    RUN_TEST(execution_snapshot_defers_save_to_merged_owner);
    RUN_TEST(execution_state_appends_preserve_both_writers_exactly_once);
}
