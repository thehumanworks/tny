/* Production-linked retrieval admission, batch rollback and resume contracts. */
#include "greatest.h"
#include "core/retrieval.h"
#include "core/retrieval_sources.h"
#include "core/tasks.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char profile_json[] =
    "{\"name\":\"retrieve\",\"provider\":\"fixture-provider\",\"model\":\"fixture-model\","
    "\"task\":\"context-retrieval\",\"description\":\"Read selected evidence\","
    "\"max_result_bytes\":8192,\"effort\":\"high\"}";

typedef struct {
    char root[512];
    char parent_dir[544];
    char child_dir[544];
    tny_ctx parent_ctx;
    tny_ctx child_ctx;
    tny_session_state parent;
    tny_session_state child;
    yyjson_doc *profile;
    yyjson_doc *input;
    yyjson_mut_doc *request;
    yyjson_mut_val *sources;
    char *parent_handles[4];
    size_t parent_count;
    char *existing_handle;
    char *wire;
    char *readback;
    char *saved_runtime;
    buf_t prompt;
    bool ready;
} retrieval_fixture;

static retrieval_fixture fixture;

static void remove_fixture_tree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *child = path_join(path, entry->d_name);
        struct stat st;
        if (child && lstat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) remove_fixture_tree(child);
            else unlink(child);
        }
        free(child);
    }
    closedir(dir);
    rmdir(path);
}

static void retrieval_setup(void *unused) {
    (void)unused;
    memset(&fixture, 0, sizeof fixture);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    int n = snprintf(fixture.root, sizeof fixture.root, "%s/tny-retrieval-test-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= sizeof fixture.root || !mkdtemp(fixture.root)) {
        fixture.root[0] = '\0';
        return;
    }
    snprintf(fixture.parent_dir, sizeof fixture.parent_dir, "%s/parent", fixture.root);
    snprintf(fixture.child_dir, sizeof fixture.child_dir, "%s/child", fixture.root);
    fixture.child_ctx.provider_name = "fixture-provider";
    fixture.child_ctx.model = "fixture-model";
    fixture.child_ctx.reasoning_effort = "high";
    fixture.child_ctx.wire_api = "responses";
    fixture.child_ctx.base_url = "https://fixture.invalid/v1";
    fixture.parent.dir = fixture.parent_dir;
    fixture.parent.ctx = &fixture.parent_ctx;
    fixture.child.dir = fixture.child_dir;
    fixture.child.ctx = &fixture.child_ctx;
    fixture.child.doc = yyjson_mut_doc_new(jallocator());
    fixture.profile = jparse(profile_json, strlen(profile_json));
    if (!fixture.child.doc || !fixture.profile) return;
    yyjson_mut_val *root = yyjson_mut_obj(fixture.child.doc);
    yyjson_mut_doc_set_root(fixture.child.doc, root);
    fixture.ready =
        root && yyjson_mut_obj_add_strcpy(fixture.child.doc, root, "title", "Existing child") &&
        tny_task_set_explicit(&fixture.child_ctx, "existing-task", "Keep this task", "explicit") ==
            0;
}

static void retrieval_teardown(void *unused) {
    (void)unused;
    yyjson_doc_free(fixture.profile);
    yyjson_doc_free(fixture.input);
    yyjson_mut_doc_free(fixture.request);
    yyjson_mut_doc_free(fixture.child.doc);
    for (size_t i = 0; i < fixture.parent_count; ++i) free(fixture.parent_handles[i]);
    free(fixture.existing_handle);
    free(fixture.wire);
    free(fixture.readback);
    free(fixture.saved_runtime);
    free(fixture.child_ctx.retrieval_state);
    free(fixture.child_ctx.task_name);
    free(fixture.child_ctx.task_source);
    free(fixture.child_ctx.task_instructions);
    buf_free(&fixture.prompt);
    if (*fixture.root) remove_fixture_tree(fixture.root);
}

static yyjson_mut_val *saved_state(void) {
    return yyjson_mut_obj_get(yyjson_mut_doc_get_root(fixture.child.doc), "retrieval");
}

static bool make_request(bool resuming) {
    yyjson_mut_doc_free(fixture.request);
    fixture.request = yyjson_mut_doc_new(jallocator());
    if (!fixture.request) return false;
    yyjson_mut_val *root = yyjson_mut_obj(fixture.request);
    yyjson_mut_doc_set_root(fixture.request, root);
    fixture.sources = yyjson_mut_arr(fixture.request);
    yyjson_mut_val *profile =
        resuming && saved_state()
            ? yyjson_mut_val_mut_copy(fixture.request, yyjson_mut_obj_get(saved_state(), "profile"))
            : yyjson_val_mut_copy(fixture.request, yyjson_doc_get_root(fixture.profile));
    return root && fixture.sources && profile &&
           yyjson_mut_obj_add_val(fixture.request, root, "profile", profile) &&
           yyjson_mut_obj_add_strcpy(fixture.request, root, "prompt", "Find the relevant bytes") &&
           yyjson_mut_obj_add_val(fixture.request, root, "sources", fixture.sources) &&
           (resuming ||
            (yyjson_mut_obj_add_strcpy(fixture.request, root, "task_body",
                                       "Cite original evidence") &&
             yyjson_mut_obj_add_strcpy(fixture.request, root, "task_source", "explicit")));
}

static yyjson_mut_val *add_source(const char *data, size_t len, const char *source) {
    if (fixture.parent_count >= sizeof fixture.parent_handles / sizeof *fixture.parent_handles)
        return NULL;
    char *handle = session_store_result(&fixture.parent, data, len);
    if (!handle) return NULL;
    fixture.parent_handles[fixture.parent_count++] = handle;
    size_t bytes = SIZE_MAX;
    yyjson_mut_val *entry = tny_retrieval_source_export(fixture.request, &fixture.parent, handle,
                                                        source, "unknown", &bytes);
    return entry && bytes == len && yyjson_mut_arr_append(fixture.sources, entry) ? entry : NULL;
}

static int admit_request(bool resuming) {
    free(fixture.wire);
    fixture.wire = yyjson_mut_write(fixture.request, 0, NULL);
    if (!fixture.wire) return -1;
    buf_free(&fixture.prompt);
    buf_init(&fixture.prompt);
    buf_appends(&fixture.prompt, fixture.wire);
    yyjson_doc_free(fixture.input);
    fixture.input = NULL;
    return fixture.prompt.oom
               ? -1
               : tny_retrieval_input(&fixture.child_ctx, &fixture.prompt, resuming, &fixture.input);
}

static int result_entries(void) {
    char path[576];
    snprintf(path, sizeof path, "%s/results", fixture.child_dir);
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT ? 0 : -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
    closedir(dir);
    return count;
}

static bool existing_result_unchanged(void) {
    size_t len = SIZE_MAX;
    free(fixture.readback);
    fixture.readback = session_read_result(&fixture.child, fixture.existing_handle, 0, 64, &len);
    return fixture.readback && len == 9 && memcmp(fixture.readback, "keep\0this", 9) == 0;
}

TEST retrieval_batch_import_publishes_owned_sources_and_resumes_without_recopies(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first\0bytes", 11, "result:parent:first"));
    ASSERT(add_source("second", 6, "result:parent:second"));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_STR_EQ("Find the relevant bytes", fixture.prompt.data);
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(2, result_entries());
    ASSERT(saved_state());
    yyjson_mut_val *sources = yyjson_mut_obj_get(saved_state(), "sources");
    ASSERT_EQ(2u, yyjson_mut_arr_size(sources));
    const char *handle =
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(sources, 0), "handle"));
    ASSERT(handle && strcmp(handle, fixture.parent_handles[0]) != 0);
    size_t bytes = SIZE_MAX;
    fixture.readback = session_read_result(&fixture.child, handle, 0, 64, &bytes);
    ASSERT(fixture.readback);
    ASSERT_EQ(11u, bytes);
    ASSERT_EQ(0, memcmp("first\0bytes", fixture.readback, bytes));
    fixture.saved_runtime = xstrdup(fixture.child_ctx.retrieval_state);
    ASSERT(fixture.saved_runtime);
    ASSERT(make_request(true));
    ASSERT_EQ(0, admit_request(true));
    ASSERT_STR_EQ("Cite original evidence", fixture.child_ctx.task_instructions);
    remove_fixture_tree(fixture.parent_dir);
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    char error[256] = {0};
    ASSERT_EQ(0, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
    ASSERT_EQ(2, result_entries());
    PASS();
}

TEST retrieval_batch_rolls_back_every_new_file_after_later_source_failure(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT(add_source("second\0bytes", 12, "result:parent:second"));
    yyjson_mut_val *last = add_source("third", 5, "result:parent:third");
    ASSERT(last);
    /* This source fails only after the first two sources have been copied. */
    ASSERT(yyjson_mut_set_str(yyjson_mut_obj_get(last, "digest"),
                              "0000000000000000000000000000000000000000000000000000000000000000"));
    fixture.existing_handle = session_store_result(&fixture.child, "keep\0this", 9);
    fixture.child_ctx.retrieval_state = xstrdup("prior runtime state");
    ASSERT(fixture.existing_handle && fixture.child_ctx.retrieval_state);
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(-1, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(1, result_entries());
    ASSERT(existing_result_unchanged());
    ASSERT_FALSE(saved_state());
    ASSERT_STR_EQ("prior runtime state", fixture.child_ctx.retrieval_state);
    ASSERT_STR_EQ("Existing child", yyjson_mut_get_str(yyjson_mut_obj_get(
                                        yyjson_mut_doc_get_root(fixture.child.doc), "title")));
    ASSERT_STR_EQ("Find the relevant bytes", fixture.prompt.data);
    /* A failed batch can be retried after replacing the bad request; no stale
     * imported handles or retrieval metadata obstruct the successful retry. */
    ASSERT(yyjson_mut_arr_remove_last(fixture.sources));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(3, result_entries());
    ASSERT(existing_result_unchanged());
    ASSERT_EQ(2u, yyjson_mut_arr_size(yyjson_mut_obj_get(saved_state(), "sources")));
    PASS();
}

TEST retrieval_batch_rolls_back_after_a_later_source_disappears(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT(add_source("second", 6, "result:parent:second"));
    yyjson_mut_val *last = add_source("third", 5, "result:parent:third");
    ASSERT(last);
    ASSERT_EQ(0, unlink(yyjson_mut_get_str(yyjson_mut_obj_get(last, "path"))));
    fixture.existing_handle = session_store_result(&fixture.child, "keep\0this", 9);
    ASSERT(fixture.existing_handle);
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(-1, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(1, result_entries());
    ASSERT(existing_result_unchanged());
    ASSERT_FALSE(saved_state());
    ASSERT_FALSE(fixture.child_ctx.retrieval_state);
    PASS();
}

TEST retrieval_batch_rejects_aggregate_overflow_without_changing_existing_files(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    yyjson_mut_val *second = add_source("second", 6, "result:parent:second");
    ASSERT(second);
    ASSERT(yyjson_mut_set_uint(yyjson_mut_obj_get(second, "bytes"), TNY_RETRIEVAL_BYTES_LIMIT));
    fixture.existing_handle = session_store_result(&fixture.child, "keep\0this", 9);
    ASSERT(fixture.existing_handle);
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(-1, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(1, result_entries());
    ASSERT(existing_result_unchanged());
    ASSERT_FALSE(saved_state());
    PASS();
}

TEST retrieval_batch_cannot_replace_an_existing_retrieval_snapshot(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    fixture.saved_runtime = xstrdup(fixture.child_ctx.retrieval_state);
    ASSERT(fixture.saved_runtime);
    yyjson_mut_val *previous = saved_state();
    ASSERT_EQ(-1, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(previous, saved_state());
    ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
    ASSERT_EQ(1, result_entries());
    PASS();
}

TEST retrieval_resume_rejects_malformed_envelopes_without_mutating_prompt_or_task(void) {
    ASSERT(fixture.ready);
    static const char *const tails[] = {
        "",
        ",\"sources\":null",
        ",\"sources\":{}",
        ",\"sources\":[0]",
        ",\"sources\":[],\"sources\":[]",
        ",\"sources\":[],\"prompt\":\"other\"",
        ",\"sources\":[],\"task_body\":null",
        ",\"sources\":[],\"task_source\":null",
        ",\"sources\":[],\"task_body\":\"new instructions\",\"task_source\":\"explicit\"",
        ",\"sources\":[],\"provider\":\"fixture-provider\"",
        ",\"sources\\u0000suffix\":[]",
        ",\"sources\":[],\"unexpected\":true",
    };
    for (size_t i = 0; i < sizeof tails / sizeof *tails; ++i) {
        buf_free(&fixture.prompt);
        buf_init(&fixture.prompt);
        buf_appendf(&fixture.prompt, "{\"profile\":%s,\"prompt\":\"continue\"%s}", profile_json,
                    tails[i]);
        free(fixture.wire);
        fixture.wire = xstrdup(fixture.prompt.data);
        ASSERT(fixture.wire);
        ASSERT_EQ(-1,
                  tny_retrieval_input(&fixture.child_ctx, &fixture.prompt, true, &fixture.input));
        ASSERT_FALSE(fixture.input);
        ASSERT_STR_EQ(fixture.wire, fixture.prompt.data);
        ASSERT_STR_EQ("existing-task", fixture.child_ctx.task_name);
        ASSERT_STR_EQ("Keep this task", fixture.child_ctx.task_instructions);
        ASSERT_EQ(0, result_entries());
    }
    static const char *const questions[] = {"null", "false", "0", "\"\"", "\"a\\u0000b\""};
    for (size_t i = 0; i < sizeof questions / sizeof *questions; ++i) {
        buf_free(&fixture.prompt);
        buf_init(&fixture.prompt);
        buf_appendf(&fixture.prompt, "{\"profile\":%s,\"prompt\":%s,\"sources\":[]}", profile_json,
                    questions[i]);
        ASSERT_EQ(-1,
                  tny_retrieval_input(&fixture.child_ctx, &fixture.prompt, true, &fixture.input));
        ASSERT_FALSE(fixture.input);
        ASSERT_STR_EQ("Keep this task", fixture.child_ctx.task_instructions);
    }
    ASSERT(make_request(true));
    ASSERT_EQ(0, admit_request(true));
    ASSERT_STR_EQ("Find the relevant bytes", fixture.prompt.data);
    ASSERT_STR_EQ("existing-task", fixture.child_ctx.task_name);
    ASSERT_STR_EQ("Keep this task", fixture.child_ctx.task_instructions);
    /* An otherwise valid resume envelope cannot create missing durable state. */
    ASSERT_EQ(-1, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    ASSERT_EQ(0, result_entries());
    PASS();
}

TEST retrieval_resume_rejects_mismatched_provider_and_model(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(true));
    fixture.child_ctx.provider_name = "different-provider";
    ASSERT_EQ(-1, admit_request(true));
    ASSERT_FALSE(fixture.input);
    fixture.child_ctx.provider_name = "fixture-provider";
    fixture.child_ctx.model = "different-model";
    ASSERT_EQ(-1, admit_request(true));
    ASSERT_FALSE(fixture.input);
    ASSERT_STR_EQ("Keep this task", fixture.child_ctx.task_instructions);
    ASSERT_EQ(0, result_entries());
    PASS();
}

TEST retrieval_resume_rejects_corrupt_saved_source_metadata(void) {
    char null_error[64] = {0};
    ASSERT_EQ(-1, tny_retrieval_reconcile(NULL, null_error, sizeof null_error));
    ASSERT_EQ(-1, tny_retrieval_reconcile(&fixture.child, NULL, sizeof null_error));
    ASSERT_EQ(-1, tny_retrieval_reconcile(&fixture.child, null_error, 0));
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    fixture.saved_runtime = xstrdup(fixture.child_ctx.retrieval_state);
    ASSERT(fixture.saved_runtime);
    yyjson_mut_val *original = saved_state();
    ASSERT(make_request(true));
    ASSERT_EQ(0, admit_request(true));
    for (size_t i = 0; i < 8; ++i) {
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(fixture.child.doc, original);
        ASSERT(copy);
        yyjson_mut_val *sources = yyjson_mut_obj_get(copy, "sources");
        yyjson_mut_val *entry = yyjson_mut_arr_get(sources, 0);
        if (i == 0) copy = yyjson_mut_null(fixture.child.doc);
        else if (i == 1) ASSERT(yyjson_mut_obj_add_bool(fixture.child.doc, copy, "extra", true));
        else if (i == 2)
            ASSERT(yyjson_mut_set_str(yyjson_mut_obj_get(entry, "handle"), "../not-a-handle"));
        else if (i == 3)
            ASSERT(yyjson_mut_set_str(
                yyjson_mut_obj_get(entry, "digest"),
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
        else if (i == 4) ASSERT(yyjson_mut_set_sint(yyjson_mut_obj_get(entry, "bytes"), -1));
        else if (i == 5)
            ASSERT(yyjson_mut_set_uint(yyjson_mut_obj_get(entry, "bytes"),
                                       (uint64_t)TNY_RETRIEVAL_BYTES_LIMIT + 1));
        else if (i == 6)
            ASSERT(yyjson_mut_obj_add_strcpy(fixture.child.doc, entry, "path", "/untrusted/path"));
        else
            for (size_t j = 0; j < TNY_RETRIEVAL_SOURCE_LIMIT; ++j)
                ASSERT(yyjson_mut_arr_append(sources,
                                             yyjson_mut_val_mut_copy(fixture.child.doc, entry)));
        ASSERT(yyjson_mut_obj_put(yyjson_mut_doc_get_root(fixture.child.doc),
                                  yyjson_mut_strcpy(fixture.child.doc, "retrieval"), copy));
        ASSERT_EQ(-1, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
        char error[256] = {0};
        ASSERT_EQ(-1, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
        ASSERT(*error);
        ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
        ASSERT_EQ(1, result_entries());
    }
    PASS();
}

TEST retrieval_reconcile_rejects_changed_or_deleted_child_copies(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    fixture.saved_runtime = xstrdup(fixture.child_ctx.retrieval_state);
    ASSERT(fixture.saved_runtime);
    yyjson_mut_val *sources = yyjson_mut_obj_get(saved_state(), "sources");
    const char *handle =
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(sources, 0), "handle"));
    ASSERT(handle);
    char path[640];
    snprintf(path, sizeof path, "%s/results/%s.txt", fixture.child_dir, handle);
    FILE *file = fopen(path, "r+b");
    ASSERT(file);
    size_t written = fwrite("other", 1, 5, file);
    int closed = fclose(file);
    ASSERT_EQ(5u, written);
    ASSERT_EQ(0, closed);
    char error[256] = {0};
    ASSERT_EQ(-1, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    ASSERT(*error);
    ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
    ASSERT_EQ(1, result_entries());
    ASSERT_EQ(0, file_write_atomic(path, "first", 5));
    ASSERT_EQ(0, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
    ASSERT_EQ(0, unlink(path));
    error[0] = '\0';
    ASSERT_EQ(-1, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    ASSERT(*error);
    ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
    ASSERT_EQ(0, result_entries());
    PASS();
}

TEST retrieval_task_reconcile_restores_old_cache_when_target_has_no_retrieval(void) {
    ASSERT(fixture.ready);
    fixture.child_ctx.retrieval_state = xstrdup("previous retrieval cache A");
    ASSERT(fixture.child_ctx.retrieval_state);
    char *previous = fixture.child_ctx.retrieval_state;
    ASSERT_FALSE(saved_state());
    ASSERT(yyjson_mut_obj_add_bool(fixture.child.doc, yyjson_mut_doc_get_root(fixture.child.doc),
                                   "task", false));
    char error[256] = {0};
    ASSERT_EQ(-1, session_task_reconcile(&fixture.child, error, sizeof error));
    ASSERT(strstr(error, "task metadata"));
    ASSERT_EQ(previous, fixture.child_ctx.retrieval_state);
    ASSERT_STR_EQ("previous retrieval cache A", fixture.child_ctx.retrieval_state);
    ASSERT_STR_EQ("existing-task", fixture.child_ctx.task_name);
    ASSERT_STR_EQ("Keep this task", fixture.child_ctx.task_instructions);
    PASS();
}

TEST retrieval_task_reconcile_restores_old_cache_after_loading_different_snapshot(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    yyjson_mut_val *target = saved_state();
    ASSERT(target);
    free(fixture.child_ctx.retrieval_state);
    fixture.child_ctx.retrieval_state = xstrdup("previous retrieval cache A");
    ASSERT(fixture.child_ctx.retrieval_state);
    char *previous = fixture.child_ctx.retrieval_state;
    ASSERT(yyjson_mut_obj_add_bool(fixture.child.doc, yyjson_mut_doc_get_root(fixture.child.doc),
                                   "task", false));
    char error[256] = {0};
    ASSERT_EQ(-1, session_task_reconcile(&fixture.child, error, sizeof error));
    ASSERT(strstr(error, "task metadata"));
    ASSERT_EQ(previous, fixture.child_ctx.retrieval_state);
    ASSERT_STR_EQ("previous retrieval cache A", fixture.child_ctx.retrieval_state);
    ASSERT_EQ(target, saved_state());
    ASSERT_EQ(1, result_entries());
    ASSERT_STR_EQ("Cite original evidence", fixture.child_ctx.task_instructions);
    PASS();
}

TEST retrieval_reconcile_pins_routing_but_allows_credential_refresh(void) {
    ASSERT(fixture.ready);
    ASSERT(make_request(false));
    ASSERT(add_source("first", 5, "result:parent:first"));
    ASSERT_EQ(0, admit_request(false));
    ASSERT_EQ(0, tny_retrieval_import(&fixture.child, fixture.input, &fixture.prompt));
    fixture.saved_runtime = xstrdup(fixture.child_ctx.retrieval_state);
    ASSERT(fixture.saved_runtime);
    char error[256] = {0};
    ASSERT_EQ(0, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    char **fields[] = {&fixture.child_ctx.provider_name, &fixture.child_ctx.model,
                       &fixture.child_ctx.base_url, &fixture.child_ctx.wire_api,
                       &fixture.child_ctx.reasoning_effort};
    char *changed[] = {"different-provider", "different-model", "https://different.invalid/v1",
                       "chat", "low"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
        char *original = *fields[i];
        *fields[i] = changed[i];
        ASSERT_EQ(-1, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
        ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
        *fields[i] = original;
        ASSERT_EQ(0, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    }
    fixture.child_ctx.api_key = "synthetic-key-before-refresh";
    ASSERT_EQ(0, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    fixture.child_ctx.api_key = "synthetic-key-after-refresh";
    ASSERT_EQ(0, tny_retrieval_reconcile(&fixture.child, error, sizeof error));
    ASSERT_STR_EQ(fixture.saved_runtime, fixture.child_ctx.retrieval_state);
    ASSERT_EQ(1, result_entries());
    PASS();
}

TEST retrieval_prepare_distinguishes_no_preset_from_failed_selection(void) {
    ASSERT(fixture.ready);
    tools_env env = {.ctx = &fixture.child_ctx, .session = &fixture.child};
    yyjson_doc *args = jparse("{}", 2);
    ASSERT(args);
    yyjson_doc *selection = NULL;
    char *error = NULL;
    bool applies = true;
    char *payload =
        tny_retrieval_prepare(&env, yyjson_doc_get_root(args), NULL, &selection, &error, &applies);
    ASSERT(!payload && !selection && !error && !applies);
    yyjson_doc_free(args);
    const char *request = "{\"preset\":\"missing\",\"prompt\":\"Read evidence\"}";
    args = jparse(request, strlen(request));
    ASSERT(args);
    payload =
        tny_retrieval_prepare(&env, yyjson_doc_get_root(args), NULL, &selection, &error, &applies);
    ASSERT(applies && !payload && !selection);
    free(error);
    error = NULL;
    yyjson_doc_free(args);
    args = jparse("{}", 2);
    ASSERT(args);
    ASSERT(yyjson_mut_obj_add_bool(fixture.child.doc, yyjson_mut_doc_get_root(fixture.child.doc),
                                   "retrieval", false));
    applies = false;
    payload = tny_retrieval_prepare(&env, yyjson_doc_get_root(args), &fixture.child, &selection,
                                    &error, &applies);
    ASSERT(applies && !payload && !selection);
    free(error);
    yyjson_doc_free(args);
    PASS();
}

SUITE(retrieval_suite) {
    SET_SETUP(retrieval_setup, NULL);
    SET_TEARDOWN(retrieval_teardown, NULL);
    RUN_TEST(retrieval_batch_import_publishes_owned_sources_and_resumes_without_recopies);
    RUN_TEST(retrieval_batch_rolls_back_every_new_file_after_later_source_failure);
    RUN_TEST(retrieval_batch_rolls_back_after_a_later_source_disappears);
    RUN_TEST(retrieval_batch_rejects_aggregate_overflow_without_changing_existing_files);
    RUN_TEST(retrieval_batch_cannot_replace_an_existing_retrieval_snapshot);
    RUN_TEST(retrieval_resume_rejects_malformed_envelopes_without_mutating_prompt_or_task);
    RUN_TEST(retrieval_resume_rejects_mismatched_provider_and_model);
    RUN_TEST(retrieval_resume_rejects_corrupt_saved_source_metadata);
    RUN_TEST(retrieval_reconcile_rejects_changed_or_deleted_child_copies);
    RUN_TEST(retrieval_task_reconcile_restores_old_cache_when_target_has_no_retrieval);
    RUN_TEST(retrieval_task_reconcile_restores_old_cache_after_loading_different_snapshot);
    RUN_TEST(retrieval_reconcile_pins_routing_but_allows_credential_refresh);
    RUN_TEST(retrieval_prepare_distinguishes_no_preset_from_failed_selection);
}
