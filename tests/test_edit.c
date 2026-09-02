/* test_edit.c — shared exact-match edit semantics (issue #96). */
#include "greatest.h"
#include "core/edit.h"
#include "util/util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *edit_temp_dir(void) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s/tny-edit-test-XXXXXX", base);
    char *dir = xstrdup(pattern);
    if (!dir || !mkdtemp(dir)) abort();
    return dir;
}

static char *edit_path(const char *dir, const char *name) {
    char *path = path_join(dir, name);
    if (!path) abort();
    return path;
}

static char *read_file(const char *path) {
    char *data = file_slurp(path, NULL);
    if (!data) abort();
    return data;
}

typedef struct {
    int calls;
    int interrupt_at;
} interrupt_probe;

static bool interrupt_after(void *userdata) {
    interrupt_probe *probe = userdata;
    probe->calls++;
    return probe->calls >= probe->interrupt_at;
}

TEST edit_one_match(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "one.txt");
    ASSERT_EQ(0, file_write_atomic(path, "before old after\n", 17));
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_OK, tny_edit_file_exact(path, "old", "new", false, NULL, &result));
    ASSERT_EQ(1, result.matches);
    ASSERT_EQ(1, result.replaced);
    char *data = read_file(path);
    ASSERT_STR_EQ("before new after\n", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_zero_reports_nearest_unique_context(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "zero.txt");
    const char *original = "alpha\ncorrect target\nomega\n";
    ASSERT_EQ(0, file_write_atomic(path, original, strlen(original)));
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_NOT_FOUND,
              tny_edit_file_exact(path, "correct targat", "replacement", false, NULL, &result));
    ASSERT_EQ(0, result.matches);
    ASSERT_EQ(2, result.nearest_line);
    ASSERT_STR_EQ("correct target", result.nearest_context);
    char *data = read_file(path);
    ASSERT_STR_EQ(original, data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_multiple_is_ambiguous_and_does_not_write(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "many.txt");
    const char *original = "old / old / old\n";
    ASSERT_EQ(0, file_write_atomic(path, original, strlen(original)));
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_AMBIGUOUS, tny_edit_file_exact(path, "old", "new", false, NULL, &result));
    ASSERT_EQ(3, result.matches);
    ASSERT_EQ(0, result.replaced);
    char *data = read_file(path);
    ASSERT_STR_EQ(original, data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_replace_all(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "all.txt");
    ASSERT_EQ(0, file_write_atomic(path, "old old old", 11));
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_OK, tny_edit_file_exact(path, "old", "x", true, NULL, &result));
    ASSERT_EQ(3, result.matches);
    ASSERT_EQ(3, result.replaced);
    char *data = read_file(path);
    ASSERT_STR_EQ("x x x", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_multiline_old(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "multi.txt");
    ASSERT_EQ(0, file_write_atomic(path, "top\none\ntwo\nbottom\n", 19));
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_OK,
              tny_edit_file_exact(path, "one\ntwo", "three\nfour", false, NULL, &result));
    char *data = read_file(path);
    ASSERT_STR_EQ("top\nthree\nfour\nbottom\n", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_preserves_crlf(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "crlf.txt");
    const char *original = "one\r\ntwo\r\nthree\r\n";
    ASSERT_EQ(0, file_write_atomic(path, original, strlen(original)));
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_OK, tny_edit_file_exact(path, "two", "second", false, NULL, &result));
    char *data = read_file(path);
    ASSERT_STR_EQ("one\r\nsecond\r\nthree\r\n", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_missing_file_is_read_error(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "missing.txt");
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_READ_ERROR, tny_edit_file_exact(path, "old", "new", false, NULL, &result));
    ASSERT_EQ(-1, access(path, F_OK));
    tny_edit_result_free(&result);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_symlink_updates_target_without_replacing_link(void) {
    char *dir = edit_temp_dir();
    char *target = edit_path(dir, "target.txt");
    char *link = edit_path(dir, "link.txt");
    ASSERT_EQ(0, file_write_atomic(target, "old\n", 4));
    if (symlink("target.txt", link) != 0) {
        unlink(target);
        rmdir(dir);
        free(link);
        free(target);
        free(dir);
        SKIP();
    }
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_OK, tny_edit_file_exact(link, "old", "new", false, NULL, &result));
    struct stat st;
    ASSERT_EQ(0, lstat(link, &st));
    ASSERT(S_ISLNK(st.st_mode));
    char *data = read_file(target);
    ASSERT_STR_EQ("new\n", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(link);
    unlink(target);
    rmdir(dir);
    free(link);
    free(target);
    free(dir);
    PASS();
}

TEST edit_relative_path_uses_current_directory(void) {
    char original_cwd[PATH_MAX];
    ASSERT(getcwd(original_cwd, sizeof original_cwd));
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "relative.txt");
    ASSERT_EQ(0, file_write_atomic(path, "old", 3));
    ASSERT_EQ(0, chdir(dir));
    tny_edit_result result = {0};
    tny_edit_status status =
        tny_edit_file_exact("relative.txt", "old", "new", false, NULL, &result);
    ASSERT_EQ(0, chdir(original_cwd));
    ASSERT_EQ(TNY_EDIT_OK, status);
    char *data = read_file(path);
    ASSERT_STR_EQ("new", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

TEST edit_interrupt_before_atomic_write_leaves_file_unchanged(void) {
    char *dir = edit_temp_dir();
    char *path = edit_path(dir, "interrupt.txt");
    ASSERT_EQ(0, file_write_atomic(path, "old", 3));
    interrupt_probe probe = {.interrupt_at = 3};
    tny_edit_hooks hooks = {.interrupted = interrupt_after, .interrupted_userdata = &probe};
    tny_edit_result result = {0};
    ASSERT_EQ(TNY_EDIT_INTERRUPTED,
              tny_edit_file_exact(path, "old", "new", false, &hooks, &result));
    char *data = read_file(path);
    ASSERT_STR_EQ("old", data);
    free(data);
    tny_edit_result_free(&result);
    unlink(path);
    rmdir(dir);
    free(path);
    free(dir);
    PASS();
}

SUITE(edit_suite) {
    RUN_TEST(edit_one_match);
    RUN_TEST(edit_zero_reports_nearest_unique_context);
    RUN_TEST(edit_multiple_is_ambiguous_and_does_not_write);
    RUN_TEST(edit_replace_all);
    RUN_TEST(edit_multiline_old);
    RUN_TEST(edit_preserves_crlf);
    RUN_TEST(edit_missing_file_is_read_error);
    RUN_TEST(edit_symlink_updates_target_without_replacing_link);
    RUN_TEST(edit_relative_path_uses_current_directory);
    RUN_TEST(edit_interrupt_before_atomic_write_leaves_file_unchanged);
}
