/* Retrieval source handoff: bounded snapshots, byte identity and publication. */
#include "greatest.h"
#include "core/retrieval_sources.h"
#include "util/util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char root[512];
    char parent_dir[544];
    char child_dir[544];
    char source_path[640];
    tny_ctx parent_ctx;
    tny_ctx child_ctx;
    tny_session_state parent;
    tny_session_state child;
    yyjson_mut_doc *export_doc;
    yyjson_mut_doc *import_doc;
    yyjson_doc *manifest;
    yyjson_mut_val *exported;
    char *handle;
    char *child_handle;
    char *readback;
    size_t exported_bytes;
    bool ready;
} source_fixture;

static source_fixture fixture;

/* Only called on this suite's private mkdtemp tree; do not follow symlinks. */
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

static void source_setup(void *unused) {
    (void)unused;
    memset(&fixture, 0, sizeof fixture);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    int n = snprintf(fixture.root, sizeof fixture.root, "%s/tny-source-test-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= sizeof fixture.root || !mkdtemp(fixture.root)) {
        fixture.root[0] = '\0';
        return;
    }
    snprintf(fixture.parent_dir, sizeof fixture.parent_dir, "%s/parent", fixture.root);
    snprintf(fixture.child_dir, sizeof fixture.child_dir, "%s/child", fixture.root);
    fixture.parent.dir = fixture.parent_dir;
    fixture.parent.ctx = &fixture.parent_ctx;
    fixture.child.dir = fixture.child_dir;
    fixture.child.ctx = &fixture.child_ctx;
    fixture.export_doc = yyjson_mut_doc_new(jallocator());
    fixture.import_doc = yyjson_mut_doc_new(jallocator());
    fixture.ready = fixture.export_doc && fixture.import_doc;
}

static void source_teardown(void *unused) {
    (void)unused;
    yyjson_doc_free(fixture.manifest);
    yyjson_mut_doc_free(fixture.export_doc);
    yyjson_mut_doc_free(fixture.import_doc);
    free(fixture.handle);
    free(fixture.child_handle);
    free(fixture.readback);
    if (*fixture.root) remove_fixture_tree(fixture.root);
}

static bool freeze_manifest(void) {
    yyjson_doc_free(fixture.manifest);
    fixture.manifest = yyjson_mut_val_imut_copy(fixture.exported, jallocator());
    return fixture.manifest != NULL;
}

static bool export_fixture(const void *data, size_t len) {
    free(fixture.handle);
    fixture.handle = session_store_result(&fixture.parent, data, len);
    if (!fixture.handle) return false;
    fixture.exported_bytes = SIZE_MAX;
    fixture.exported =
        tny_retrieval_source_export(fixture.export_doc, &fixture.parent, fixture.handle,
                                    "fixture stdout", "complete bytes", &fixture.exported_bytes);
    if (!fixture.exported || !freeze_manifest()) return false;
    const char *path = jget_str(yyjson_doc_get_root(fixture.manifest), "path");
    if (!path) return false;
    int n = snprintf(fixture.source_path, sizeof fixture.source_path, "%s", path);
    return n >= 0 && (size_t)n < sizeof fixture.source_path;
}

static yyjson_mut_val *import_fixture(size_t *bytes) {
    return tny_retrieval_source_import(fixture.import_doc, &fixture.child,
                                       yyjson_doc_get_root(fixture.manifest), bytes);
}

/* Count all entries, including unpublished temporary files. */
static int child_result_entries(void) {
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

TEST retrieval_sources_roundtrip_binary_empty_and_block_boundaries(void) {
    ASSERT(fixture.ready);
    static unsigned char payload[2 * 65536 + 137];
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = (unsigned char)(i * 37 + 11);
    /* Independent SHA256-chain-v1 vectors: previous digest starts at 32 zero
     * bytes; each nonempty block is at most 65536 bytes. */
    static const struct {
        size_t len;
        const char *digest;
    } vectors[] = {
        {0, "0000000000000000000000000000000000000000000000000000000000000000"},
        {8, "a02098f542c201dc44b06e2f3145b8dcb3745d0271af9fc9436256600e9394b9"},
        {65536, "0c4c7e008cead3ff1b0b0a2bbbfa494291c0ad8bc94ff1705cde39ce54983cf1"},
        {65537, "ca8ad7d4a7c3ea0a5f743b0f42f6020fe1fc36a7bb7457678a6ae61b6eb02bda"},
        {sizeof payload, "e896b55a139319a1789d242489f9f121b86327e800025c0ca0b7d712244fa4c4"},
    };
    for (size_t i = 0; i < sizeof vectors / sizeof *vectors; ++i) {
        ASSERT(export_fixture(payload, vectors[i].len));
        ASSERT_EQ(vectors[i].len, fixture.exported_bytes);
        yyjson_val *manifest = yyjson_doc_get_root(fixture.manifest);
        ASSERT_STR_EQ(vectors[i].digest, jget_str(manifest, "digest"));
        ASSERT_EQ(vectors[i].len, yyjson_get_uint(jget(manifest, "bytes")));
        size_t bytes = SIZE_MAX;
        yyjson_mut_val *result = import_fixture(&bytes);
        ASSERT(result);
        ASSERT_EQ(vectors[i].len, bytes);
        ASSERT_EQ(vectors[i].len, yyjson_mut_get_uint(yyjson_mut_obj_get(result, "bytes")));
        ASSERT_STR_EQ("fixture stdout", yyjson_mut_get_str(yyjson_mut_obj_get(result, "source")));
        ASSERT_STR_EQ("complete bytes", yyjson_mut_get_str(yyjson_mut_obj_get(result, "coverage")));
        ASSERT_STR_EQ(vectors[i].digest, yyjson_mut_get_str(yyjson_mut_obj_get(result, "digest")));
        ASSERT_FALSE(yyjson_mut_obj_get(result, "path"));
        const char *handle = yyjson_mut_get_str(yyjson_mut_obj_get(result, "handle"));
        ASSERT(handle && strlen(handle) == 16);
        ASSERT(strcmp(handle, fixture.handle) != 0);
        free(fixture.readback);
        size_t got = SIZE_MAX;
        fixture.readback = session_read_result(&fixture.child, handle, 0, vectors[i].len + 1, &got);
        ASSERT(fixture.readback);
        ASSERT_EQ(vectors[i].len, got);
        ASSERT_EQ(0, memcmp(payload, fixture.readback, got));
        ASSERT_EQ((int)i + 1, child_result_entries());
    }
    PASS();
}

TEST retrieval_sources_child_bytes_survive_parent_deletion(void) {
    ASSERT(fixture.ready);
    static const char payload[] = "a\0b\xff\noriginal source bytes";
    ASSERT(export_fixture(payload, sizeof payload));
    size_t bytes = SIZE_MAX;
    yyjson_mut_val *result = import_fixture(&bytes);
    ASSERT(result);
    const char *handle = yyjson_mut_get_str(yyjson_mut_obj_get(result, "handle"));
    ASSERT(handle);
    char child_path[640];
    snprintf(child_path, sizeof child_path, "%s/results/%s.txt", fixture.child_dir, handle);
    struct stat parent_stat, child_stat;
    ASSERT_EQ(0, stat(fixture.source_path, &parent_stat));
    ASSERT_EQ(0, lstat(child_path, &child_stat));
    ASSERT(S_ISREG(child_stat.st_mode));
    ASSERT(parent_stat.st_dev != child_stat.st_dev || parent_stat.st_ino != child_stat.st_ino);
    remove_fixture_tree(fixture.parent_dir);
    ASSERT(access(fixture.parent_dir, F_OK) != 0);
    size_t got = SIZE_MAX;
    fixture.readback = session_read_result(&fixture.child, handle, 0, sizeof payload + 1, &got);
    ASSERT(fixture.readback);
    ASSERT_EQ(sizeof payload, got);
    ASSERT_EQ(0, memcmp(payload, fixture.readback, sizeof payload));
    PASS();
}

TEST retrieval_sources_reject_deleted_source_without_publication(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    ASSERT_EQ(0, unlink(fixture.source_path));
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    PASS();
}

TEST retrieval_sources_reject_same_size_change_and_preserve_existing_child_results(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    fixture.child_handle = session_store_result(&fixture.child, "keep\0me", 7);
    ASSERT(fixture.child_handle);
    /* Same size and inode, with timestamps restored where possible: digest
     * verification, not just size/mtime, must detect altered source bytes. */
    struct stat before;
    ASSERT_EQ(0, stat(fixture.source_path, &before));
    FILE *source = fopen(fixture.source_path, "r+b");
    ASSERT(source);
    size_t written = fwrite("modified", 1, 8, source);
    int closed = fclose(source);
    ASSERT_EQ(8u, written);
    ASSERT_EQ(0, closed);
    struct timespec times[2] = {{before.st_atime, 0}, {before.st_mtime, 0}};
    ASSERT_EQ(0, utimensat(AT_FDCWD, fixture.source_path, times, 0));
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(1, child_result_entries());
    size_t got = SIZE_MAX;
    fixture.readback = session_read_result(&fixture.child, fixture.child_handle, 0, 8, &got);
    ASSERT(fixture.readback);
    ASSERT_EQ(7u, got);
    ASSERT_EQ(0, memcmp("keep\0me", fixture.readback, got));
    PASS();
}

TEST retrieval_sources_reject_growing_and_shrinking_source(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    const off_t sizes[] = {9, 7, 0};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        ASSERT_EQ(0, truncate(fixture.source_path, sizes[i]));
        size_t bytes = SIZE_MAX;
        ASSERT_FALSE(import_fixture(&bytes));
        ASSERT_EQ(SIZE_MAX, bytes);
        ASSERT_EQ(0, child_result_entries());
    }
    PASS();
}

TEST retrieval_sources_reject_symlink_substitution_on_export_and_import(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    char other[576];
    snprintf(other, sizeof other, "%s/unchanged-copy", fixture.root);
    ASSERT_EQ(0, rename(fixture.source_path, other));
    ASSERT_EQ(0, symlink(other, fixture.source_path));
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.parent, fixture.handle,
                                             "fixture stdout", "complete bytes", &bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    PASS();
}

TEST retrieval_sources_reject_nonregular_source_without_blocking(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    ASSERT_EQ(0, unlink(fixture.source_path));
    ASSERT_EQ(0, mkfifo(fixture.source_path, 0600));
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.parent, fixture.handle,
                                             "fixture stdout", "complete bytes", &bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, unlink(fixture.source_path));
    ASSERT_EQ(0, mkdir(fixture.source_path, 0700));
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.parent, fixture.handle,
                                             "fixture stdout", "complete bytes", &bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    PASS();
}

TEST retrieval_sources_reject_invalid_or_foreign_handles(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    const char *invalid[] = {NULL,
                             "",
                             "abcd",
                             "000000000000000",
                             "00000000000000000",
                             "ABCDEF0123456789",
                             "000000000000000g",
                             "../../secret",
                             "/tmp/source",
                             "../0000000000000",
                             "transcript:0:0000000000000000"};
    size_t bytes = SIZE_MAX;
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.parent, invalid[i],
                                                 "fixture stdout", "complete bytes", &bytes));
        ASSERT_EQ(SIZE_MAX, bytes);
    }
    /* A syntactically valid handle is not authority to read another session. */
    ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.child, fixture.handle,
                                             "fixture stdout", "complete bytes", &bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    PASS();
}

TEST retrieval_sources_reject_no_save_without_creating_artifacts(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    fixture.parent_ctx.no_save = true;
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.parent, fixture.handle,
                                             "fixture stdout", "complete bytes", &bytes));
    fixture.child_ctx.no_save = true;
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    ASSERT(access(fixture.child_dir, F_OK) != 0);
    ASSERT_EQ(0, fixture.child.n_mem_results);
    PASS();
}

TEST retrieval_sources_reject_oversize_before_copying(void) {
    ASSERT(fixture.ready);
    ASSERT_EQ(32u, TNY_RETRIEVAL_SOURCE_LIMIT);
    ASSERT_EQ(64u * 1024u * 1024u, TNY_RETRIEVAL_BYTES_LIMIT);
    ASSERT(export_fixture("original", 8));
    /* Sparse fixture exercises the size preflight without allocating 64 MiB. */
    ASSERT_EQ(0, truncate(fixture.source_path, (off_t)TNY_RETRIEVAL_BYTES_LIMIT + 1));
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(tny_retrieval_source_export(fixture.export_doc, &fixture.parent, fixture.handle,
                                             "fixture stdout", "complete bytes", &bytes));
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    ASSERT(access(fixture.child_dir, F_OK) != 0);
    ASSERT_EQ(0, truncate(fixture.source_path, 8));
    ASSERT(yyjson_mut_set_uint(yyjson_mut_obj_get(fixture.exported, "bytes"),
                               (uint64_t)TNY_RETRIEVAL_BYTES_LIMIT + 1));
    ASSERT(freeze_manifest());
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT(access(fixture.child_dir, F_OK) != 0);
    PASS();
}

TEST retrieval_sources_reject_bad_digest_without_leaving_temporary_files(void) {
    ASSERT(fixture.ready);
    ASSERT(export_fixture("original", 8));
    ASSERT(yyjson_mut_set_str(yyjson_mut_obj_get(fixture.exported, "digest"),
                              "0000000000000000000000000000000000000000000000000000000000000000"));
    ASSERT(freeze_manifest());
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(import_fixture(&bytes));
    ASSERT_EQ(SIZE_MAX, bytes);
    ASSERT_EQ(0, child_result_entries());
    PASS();
}

TEST retrieval_sources_typed_writer_owns_atomic_publication(void) {
    ASSERT(fixture.ready);
    size_t bytes = SIZE_MAX;
    ASSERT_FALSE(session_result_stream_finish_handle(NULL, &bytes));
    ASSERT_EQ(0u, bytes);
    session_result_stream *stream = session_result_stream_new(&fixture.child, 0);
    ASSERT(stream);
    session_result_stream_append(stream, "partial", 7);
    ASSERT_FALSE(session_result_stream_finish_handle(stream, NULL));
    ASSERT_EQ(0, child_result_entries());
    stream = session_result_stream_new(&fixture.child, 0);
    ASSERT(stream);
    session_result_stream_append(stream, "aborted", 7);
    session_result_stream_abort(stream);
    ASSERT_EQ(0, child_result_entries());
    stream = session_result_stream_new(&fixture.child, 0);
    ASSERT(stream);
    fixture.child_handle = session_result_stream_finish_handle(stream, &bytes);
    ASSERT(fixture.child_handle);
    ASSERT_EQ(0u, bytes);
    ASSERT_EQ(1, child_result_entries());
    tny_retrieval_source_discard(&fixture.child, fixture.child_handle);
    ASSERT_EQ(0, child_result_entries());
    PASS();
}

SUITE(retrieval_sources_suite) {
    SET_SETUP(source_setup, NULL);
    SET_TEARDOWN(source_teardown, NULL);
    RUN_TEST(retrieval_sources_typed_writer_owns_atomic_publication);
    RUN_TEST(retrieval_sources_roundtrip_binary_empty_and_block_boundaries);
    RUN_TEST(retrieval_sources_child_bytes_survive_parent_deletion);
    RUN_TEST(retrieval_sources_reject_deleted_source_without_publication);
    RUN_TEST(retrieval_sources_reject_same_size_change_and_preserve_existing_child_results);
    RUN_TEST(retrieval_sources_reject_growing_and_shrinking_source);
    RUN_TEST(retrieval_sources_reject_symlink_substitution_on_export_and_import);
    RUN_TEST(retrieval_sources_reject_nonregular_source_without_blocking);
    RUN_TEST(retrieval_sources_reject_invalid_or_foreign_handles);
    RUN_TEST(retrieval_sources_reject_no_save_without_creating_artifacts);
    RUN_TEST(retrieval_sources_reject_oversize_before_copying);
    RUN_TEST(retrieval_sources_reject_bad_digest_without_leaving_temporary_files);
}
