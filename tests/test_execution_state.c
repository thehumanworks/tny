#include "greatest.h"
#include "core/execution_state.h"
#include "core/image.h"
#include "util/image_io.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    const char *input = "{\"messages\":[],\"base\":1,\"removed\":true}";
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
    const char *marker = disk ? jget_str(yyjson_doc_get_root(disk), "server_change") : NULL;
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
    ASSERT(yyjson_mut_obj_add_strcpy(child.session->doc, root, "undo", "server-change"));
    yyjson_mut_obj_remove_key(root, "removed");
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
    ASSERT_STR_EQ("server-change", yyjson_mut_get_str(yyjson_mut_obj_get(root, "undo")));
    ASSERT_STR_EQ("parent-change", yyjson_mut_get_str(yyjson_mut_obj_get(root, "callback")));
    ASSERT(yyjson_mut_obj_get(root, "removed") == NULL);
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
    ASSERT(yyjson_mut_obj_add_int(child.session->doc, yyjson_mut_doc_get_root(child.session->doc),
                                  "changed", 5));
    perm_grant(child.perm, "read_file", "/new-grant");
    ASSERT(state_image(&child, "/never-reopened.png"));
    child.pending_capture[0].sha256[0] = child.pending_capture[0].sha256[0] == 'a' ? 'b' : 'a';
    yyjson_doc *delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    yyjson_mut_doc *original_doc = parent.session->doc;
    ASSERT_FALSE(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    ASSERT(parent.session->doc == original_doc);
    ASSERT(yyjson_mut_obj_get(yyjson_mut_doc_get_root(original_doc), "changed") == NULL);
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
    ASSERT(yyjson_mut_obj_add_strcpy(child.session->doc,
                                     yyjson_mut_doc_get_root(child.session->doc), "server_change",
                                     "merged-before-ack"));
    ASSERT_EQ(-1, session_save(child.session));
    char *path = path_join(directory, "session.json");
    ASSERT(path);
    yyjson_doc *disk = jparse_file(path);
    ASSERT(disk);
    ASSERT_STR_EQ("preserve-on-disk", jget_str(yyjson_doc_get_root(disk), "owner_hook"));
    ASSERT(jget(yyjson_doc_get_root(disk), "server_change") == NULL);
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
    ASSERT_STR_EQ("merged-before-ack", jget_str(yyjson_doc_get_root(disk), "server_change"));
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
    ASSERT(yyjson_mut_obj_add_bool(child.session->doc, yyjson_mut_doc_get_root(child.session->doc),
                                   "late", true));
    delta = state_snapshot(&child, yyjson_doc_get_root(baseline), false);
    ASSERT(delta);
    ASSERT_FALSE(tny_execution_state_apply(&parent, yyjson_doc_get_root(delta), false));
    ASSERT(yyjson_mut_get_bool(
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(parent.session->doc), "late")));
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
SUITE(execution_state_suite) {
    RUN_TEST(execution_state_initial_and_delta_preserve_parent);
    RUN_TEST(execution_state_failed_apply_is_atomic);
    RUN_TEST(execution_state_null_session);
    RUN_TEST(execution_snapshot_defers_save_to_merged_owner);
    RUN_TEST(execution_state_appends_preserve_both_writers_exactly_once);
}
