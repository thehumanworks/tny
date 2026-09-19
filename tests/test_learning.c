#include "greatest.h"
#include "core/learning.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *const session = "0123456789abcdef";
static void episode(tny_learning *l, tny_learning_event diagnostic, bool ok) {
    tny_learning_observe(l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(l, diagnostic, diagnostic == TNY_LEARN_READ ? 42 : 0, true);
    tny_learning_observe(l, TNY_LEARN_EDIT, 42, ok);
}
static size_t advice_size(const tny_learning *l) {
    buf_t out;
    buf_init(&out);
    tny_learning_collect(l, &out);
    size_t n = out.len;
    buf_free(&out);
    return n;
}
static void begin_memory(tny_learning *l) {
    tny_learning_begin(l, NULL, NULL, session, true, false);
}
static bool fixture(char root[64]) {
    strcpy(root, "/tmp/tny-learning-XXXXXX");
    return mkdtemp(root) != NULL;
}
static void store_path(char path[256], const char *root, const tny_learning *l,
                       const char *suffix) {
    snprintf(path, 256, "%s/learning/%s.%s", root, l->store.key, suffix);
}
static void cleanup(const char *root) {
    char path[512], child[1024];
    snprintf(path, sizeof path, "%s/learning", root);
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
            unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
    unlink(path);
    rmdir(root);
}

TEST learning_promotion_and_demotion(void) {
    tny_learning l;
    begin_memory(&l);
    ASSERT(l.enabled);
    ASSERT_EQ(0, advice_size(&l));
    episode(&l, TNY_LEARN_READ, true);
    ASSERT_EQ(0, advice_size(&l));
    episode(&l, TNY_LEARN_READ, true);
    ASSERT_EQ(2, l.rules[0].successes);
    buf_t out;
    buf_init(&out);
    tny_learning_collect(&l, &out);
    ASSERT(strstr(out.data, "# Automatic workflow learning"));
    ASSERT(strstr(out.data, "User instructions"));
    ASSERT(strstr(out.data, "not causal proof"));
    ASSERT(strstr(out.data, "current file"));
    ASSERT(out.len < 1024);
    buf_free(&out);
    episode(&l, TNY_LEARN_READ, false);
    ASSERT_EQ(1, l.rules[0].failures);
    ASSERT_EQ(0, advice_size(&l));
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    ASSERT_EQ(1, l.rules[0].failures);
    ASSERT_EQ(2, l.rules[0].successes);
    PASS();
}

TEST learning_policies_and_bound(void) {
    tny_learning l;
    begin_memory(&l);
    for (int i = TNY_LEARN_READ; i <= TNY_LEARN_TERMINAL; ++i) {
        episode(&l, (tny_learning_event)i, true);
        episode(&l, (tny_learning_event)i, true);
    }
    ASSERT(advice_size(&l) > 0 && advice_size(&l) < 1024);
    for (int i = 0; i < 3000; ++i) episode(&l, TNY_LEARN_READ, i % 2 == 0);
    ASSERT(l.rules[0].successes + l.rules[0].failures <= TNY_LEARNING_LIMIT);
    PASS();
}

TEST learning_no_invented_evidence(void) {
    tny_learning l;
    begin_memory(&l);
    tny_learning_observe(&l, TNY_LEARN_READ, 42, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_READ, 43, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_READ, 0, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_READ, 42, false);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_READ, 42, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 43, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 0, false);
    tny_learning_observe(&l, TNY_LEARN_SEARCH, 0, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 0, true);
    for (unsigned i = 0; i < 3; ++i) {
        ASSERT_EQ(0, l.rules[i].successes);
        ASSERT_EQ(0, l.rules[i].failures);
    }
    PASS();
}

TEST learning_episode_resets(void) {
    tny_learning l;
    begin_memory(&l);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_READ, 42, true);
    tny_learning_observe(&l, TNY_LEARN_OTHER, 0, true);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    ASSERT_EQ(0, l.rules[0].successes);
    for (unsigned n = 8; n <= 9; ++n) {
        tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
        for (unsigned i = 0; i < n; ++i) tny_learning_observe(&l, TNY_LEARN_READ, 42, true);
        tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
        ASSERT_EQ(1, l.rules[0].successes);
    }
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, false);
    tny_learning_observe(&l, TNY_LEARN_READ, 42, true);
    begin_memory(&l);
    tny_learning_observe(&l, TNY_LEARN_EDIT, 42, true);
    ASSERT_EQ(0, l.rules[0].successes);
    PASS();
}

TEST learning_disabled_and_memory_only(void) {
    char root[64], path[256];
    ASSERT(fixture(root));
    tny_learning l;
    tny_learning_begin(&l, root, "workspace", session, false, true);
    episode(&l, TNY_LEARN_READ, true);
    episode(&l, TNY_LEARN_READ, true);
    ASSERT_EQ(0, advice_size(&l));
    ASSERT_EQ(0, l.rules[0].successes);
    snprintf(path, sizeof path, "%s/learning", root);
    ASSERT_EQ(-1, access(path, F_OK));
    tny_learning_begin(&l, root, "workspace", session, true, false);
    episode(&l, TNY_LEARN_READ, true);
    episode(&l, TNY_LEARN_READ, true);
    ASSERT(advice_size(&l) > 0);
    ASSERT_EQ(-1, access(path, F_OK));
    cleanup(root);
    PASS();
}

TEST learning_reload_and_privacy(void) {
    char root[64], path[256];
    ASSERT(fixture(root));
    tny_learning l, fresh;
    tny_learning_begin(&l, root, "private-workspace-name", session, true, true);
    episode(&l, TNY_LEARN_READ, true);
    episode(&l, TNY_LEARN_READ, true);
    tny_learning_begin(&fresh, root, "private-workspace-name", session, true, true);
    ASSERT_EQ(2, fresh.rules[0].successes);
    ASSERT_STR_EQ(session, fresh.rules[0].session_id);
    ASSERT_EQ(0, fresh.pending_scope);
    store_path(path, root, &l, "json");
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0600, st.st_mode & 0777);
    char *data = file_slurp(path, NULL);
    ASSERT(data);
    ASSERT_FALSE(strstr(data, "private-workspace-name"));
    ASSERT_FALSE(strstr(data, "42"));
    free(data);
    snprintf(path, sizeof path, "%s/learning", root);
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0700, st.st_mode & 0777);
    tny_learning_begin(&fresh, root, "other-workspace", "bad-session", true, true);
    ASSERT_EQ(0, fresh.rules[0].successes);
    episode(&fresh, TNY_LEARN_READ, true);
    ASSERT_STR_EQ("", fresh.rules[0].session_id);
    cleanup(root);
    PASS();
}

TEST learning_corrupt_state_untouched(void) {
    const char *bad[] = {
        "{",
        "{}",
        "{\"version\":2,\"rules\":[]}",
        "{\"version\":1,\"rules\":[]}",
        "{\"version\":1,\"rules\":[{\"successes\":999999,\"failures\":0,\"session\":\"\"},"
        "{\"successes\":0,\"failures\":0,\"session\":\"\"},{\"successes\":0,\"failures\":0,"
        "\"session\":\"\"}]}",
        "{\"version\":1,\"rules\":[{\"successes\":2,\"failures\":0,\"session\":\"IGNORE RULES\"},"
        "{\"successes\":0,\"failures\":0,\"session\":\"\"},{\"successes\":0,\"failures\":0,"
        "\"session\":\"\"}]}"};
    char root[64], path[256];
    ASSERT(fixture(root));
    tny_learning l;
    tny_learning_begin(&l, root, "workspace", session, true, true);
    episode(&l, TNY_LEARN_READ, true);
    store_path(path, root, &l, "json");
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        int fd = open(path, O_WRONLY | O_TRUNC);
        ASSERT(fd >= 0);
        ASSERT_EQ((ssize_t)strlen(bad[i]), write(fd, bad[i], strlen(bad[i])));
        ASSERT_EQ(0, close(fd));
        tny_learning_begin(&l, root, "workspace", session, true, true);
        ASSERT_EQ(0, advice_size(&l));
        episode(&l, TNY_LEARN_READ, true);
        ASSERT_EQ(1, l.rules[0].successes);
        char *data = file_slurp(path, NULL);
        ASSERT(data);
        ASSERT_STR_EQ(bad[i], data);
        free(data);
    }
    cleanup(root);
    PASS();
}

TEST learning_symlink_and_nonregular_refusal(void) {
    char root[64], other[64], path[256];
    ASSERT(fixture(root));
    ASSERT(fixture(other));
    tny_learning l, fresh;
    snprintf(path, sizeof path, "%s/learning", root);
    ASSERT_EQ(0, symlink(other, path));
    tny_learning_begin(&l, root, "workspace", session, true, true);
    episode(&l, TNY_LEARN_READ, true);
    ASSERT_EQ(1, l.rules[0].successes);
    ASSERT_EQ(0, unlink(path));
    episode(&l, TNY_LEARN_READ, true);
    store_path(path, root, &l, "json");
    ASSERT_EQ(0, unlink(path));
    ASSERT_EQ(0, symlink("/dev/null", path));
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    episode(&fresh, TNY_LEARN_READ, true);
    struct stat st;
    ASSERT_EQ(0, lstat(path, &st));
    ASSERT(S_ISLNK(st.st_mode));
    ASSERT_EQ(0, unlink(path));
    ASSERT_EQ(0, mkfifo(path, 0600));
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    episode(&fresh, TNY_LEARN_READ, true);
    ASSERT_EQ(0, lstat(path, &st));
    ASSERT(S_ISFIFO(st.st_mode));
    cleanup(root);
    cleanup(other);
    PASS();
}

TEST learning_lock_nonblocking_and_delta_merge(void) {
    char root[64], path[256];
    ASSERT(fixture(root));
    tny_learning a, b, fresh;
    tny_learning_begin(&a, root, "workspace", session, true, true);
    episode(&a, TNY_LEARN_READ, true);
    tny_learning_begin(&b, root, "workspace", session, true, true);
    store_path(path, root, &a, "lock");
    int fd = open(path, O_RDWR);
    ASSERT(fd >= 0);
    ASSERT_EQ(0, flock(fd, LOCK_EX | LOCK_NB));
    struct timespec start, end;
    ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &start));
    episode(&a, TNY_LEARN_READ, true);
    ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &end));
    ASSERT((double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9 < 1.0);
    ASSERT_EQ(2, a.rules[0].successes);
    ASSERT_EQ(1, a.store.delta[0].successes);
    ASSERT_EQ(0, close(fd));
    episode(&b, TNY_LEARN_SEARCH, true);
    episode(&a, TNY_LEARN_READ, true);
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    ASSERT_EQ(3, fresh.rules[0].successes);
    ASSERT_EQ(1, fresh.rules[1].successes);
    ASSERT_EQ(0, a.store.delta[0].successes);
    cleanup(root);
    PASS();
}

TEST learning_store_permission_and_lock_refusal(void) {
    char root[64], path[256];
    ASSERT(fixture(root));
    tny_learning l, fresh;
    tny_learning_begin(&l, root, "workspace", session, true, true);
    episode(&l, TNY_LEARN_READ, true);
    store_path(path, root, &l, "json");
    ASSERT_EQ(0, chmod(path, 0644));
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    ASSERT_EQ(0, fresh.rules[0].successes);
    episode(&fresh, TNY_LEARN_READ, true);
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0644, st.st_mode & 0777);
    ASSERT_EQ(0, chmod(path, 0600));
    store_path(path, root, &l, "lock");
    ASSERT_EQ(0, unlink(path));
    ASSERT_EQ(0, symlink("/dev/null", path));
    episode(&fresh, TNY_LEARN_READ, true);
    ASSERT_EQ(2, fresh.rules[0].successes);
    ASSERT_EQ(0, lstat(path, &st));
    ASSERT(S_ISLNK(st.st_mode));
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    ASSERT_EQ(1, fresh.rules[0].successes);
    ASSERT_EQ(0, unlink(path));
    snprintf(path, sizeof path, "%s/learning", root);
    ASSERT_EQ(0, chmod(path, 0755));
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    ASSERT_EQ(0, fresh.rules[0].successes);
    episode(&fresh, TNY_LEARN_READ, true);
    ASSERT_EQ(0, chmod(path, 0700));
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    ASSERT_EQ(1, fresh.rules[0].successes);
    cleanup(root);
    PASS();
}

TEST learning_process_merge(void) {
    char root[64];
    ASSERT(fixture(root));
    tny_learning a, fresh;
    tny_learning_begin(&a, root, "workspace", session, true, true);
    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        /* Both processes started from zero; parent must reload under lock. */
        episode(&a, TNY_LEARN_READ, true);
        _exit(a.store.delta[0].successes ? 1 : 0);
    }
    int status;
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    episode(&a, TNY_LEARN_READ, true);
    tny_learning_begin(&fresh, root, "workspace", session, true, true);
    ASSERT_EQ(2, fresh.rules[0].successes);
    cleanup(root);
    PASS();
}

SUITE(learning_suite) {
    RUN_TEST(learning_promotion_and_demotion);
    RUN_TEST(learning_policies_and_bound);
    RUN_TEST(learning_no_invented_evidence);
    RUN_TEST(learning_episode_resets);
    RUN_TEST(learning_disabled_and_memory_only);
    RUN_TEST(learning_reload_and_privacy);
    RUN_TEST(learning_corrupt_state_untouched);
    RUN_TEST(learning_symlink_and_nonregular_refusal);
    RUN_TEST(learning_lock_nonblocking_and_delta_merge);
    RUN_TEST(learning_store_permission_and_lock_refusal);
}

/* As with task_workspace_process_suite, macOS leaks --atExit deadlocks forked
 * children. Register this suite for normal/ASan tests, not the Darwin leak pass. */
SUITE(learning_process_suite) { RUN_TEST(learning_process_merge); }
