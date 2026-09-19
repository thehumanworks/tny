#include "greatest.h"
#include "util/task_workspace.h"
#include "util/git.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define GIT(p, ...) git_run(p, (const char *const[]){__VA_ARGS__, NULL}, &output)
static buf_t output;
static char err[512];
static const char *run = "0123456789abcdef0123456789abcdef";
static task_workspace_id identity(int task) { return (task_workspace_id){run, task, 1}; }
static char *fixture(void) {
    char temp[] = "/tmp/tny-task-test-XXXXXX";
    if (!mkdtemp(temp)) return NULL;
    char *p = realpath(temp, NULL);
    if (!p) return NULL;
    if (GIT(p, "init", "-b", "main") || GIT(p, "config", "user.email", "fixture@example.invalid") ||
        GIT(p, "config", "user.name", "Fixture")) {
        fprintf(stderr, "workspace fixture requires Git init/config: %s\n",
                output.data ? output.data : "no Git output");
        free(p);
        return NULL;
    }
    char *file = path_join(p, "same.txt");
    int rc = file ? file_write_atomic(file, "base\n", 5) : -1;
    free(file);
    if (rc || GIT(p, "add", "same.txt") || GIT(p, "commit", "-m", "base")) {
        free(p);
        return NULL;
    }
    return p;
}
static int write_file(const char *p, const char *name, const char *text) {
    char *file = path_join(p, name);
    int rc = file ? file_write_atomic(file, text, strlen(text)) : -1;
    free(file);
    return rc;
}
static int edit_pair(const char *a, const char *b) {
    int gate[2];
    if (pipe(gate)) return -1;
    pid_t children[2] = {-1, -1};
    int rc = 0;
    for (size_t i = 0; i < 2; ++i) {
        children[i] = fork();
        if (children[i] == 0) {
            close(gate[1]);
            char token;
            if (read(gate[0], &token, 1) != 1) _exit(2);
            close(gate[0]);
            _exit(write_file(i ? b : a, "same.txt", i ? "worker B\n" : "worker A\n") ? 3 : 0);
        }
        if (children[i] < 0) {
            rc = -1;
            break;
        }
    }
    /* Both mock workers are alive before either receives permission to edit. */
    close(gate[0]);
    if (!rc && write(gate[1], "go", 2) != 2) rc = -1;
    close(gate[1]);
    for (size_t i = 0; i < 2; ++i) {
        if (children[i] < 0) continue;
        int status = 0;
        if (waitpid(children[i], &status, 0) != children[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            rc = -1;
    }
    return rc;
}
static void finish(char *p) {
    /* Only mkdtemp-owned fixture roots reach this helper. */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf -- '%s'", p);
    int rc = system(cmd);
    if (rc) fprintf(stderr, "fixture cleanup failed: %s\n", p);
    free(p);
    buf_free(&output);
}

TEST task_workspace_identity_and_platform_refusal(void) {
    ASSERT(task_workspace_id_valid(identity(0)));
    ASSERT_FALSE(task_workspace_id_valid((task_workspace_id){"../bad", 0, 1}));
    ASSERT_FALSE(task_workspace_id_valid((task_workspace_id){run, 0, 0}));
    ASSERT_FALSE(task_workspace_id_valid((task_workspace_id){run, -1, 1}));
    ASSERT_FALSE(task_workspace_id_valid((task_workspace_id){run, 0, -1}));
    task_workspace *w = NULL;
    ASSERT_EQ(-1,
              task_workspace_prepare("ssh://host/repo", identity(0), "HEAD", &w, err, sizeof err));
    char temp[] = "/tmp/tny-task-nongit-XXXXXX";
    ASSERT(mkdtemp(temp));
    ASSERT_EQ(-1, task_workspace_prepare(temp, identity(0), "HEAD", &w, err, sizeof err));
    ASSERT_EQ(0, rmdir(temp));
    PASS();
}

TEST task_workspace_two_workers_and_conflict(void) {
    char *p = fixture();
    ASSERT(p);
    task_workspace *a = NULL, *b = NULL;
    ASSERT_EQ(0, task_workspace_prepare(p, identity(0), NULL, &a, err, sizeof err));
    ASSERT_EQ(0, task_workspace_prepare(p, identity(1), NULL, &b, err, sizeof err));
    const char *pa = task_workspace_path(a), *pb = task_workspace_path(b);
    ASSERT(strcmp(pa, pb));
    ASSERT_EQ(0, edit_pair(pa, pb));
    ASSERT_EQ(0, GIT(p, "status", "--porcelain"));
    ASSERT_EQ(0, output.len);
    ASSERT_EQ(0, GIT(p, "show", "HEAD:same.txt"));
    ASSERT_STR_EQ("base", output.data);
    task_workspace_result r;
    ASSERT_EQ(0, task_workspace_inspect(a, &r, err, sizeof err));
    ASSERT(r.dirty);
    ASSERT(strstr(r.patch, "+worker A"));
    ASSERT_STR_EQ(p, r.origin);
    ASSERT_STR_EQ(run, r.run);
    ASSERT_EQ(0, r.task);
    ASSERT_EQ(1, r.attempt);
    char *patch = path_join(p, ".git/worker.patch");
    ASSERT(patch);
    ASSERT_EQ(0, file_write_atomic(patch, r.patch, strlen(r.patch)));
    ASSERT_EQ(0, GIT(p, "apply", "--check", patch));
    free(patch);
    task_workspace_result_free(&r);
    ASSERT_EQ(-1, task_workspace_cleanup(a, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_integrate(a, err, sizeof err));
    ASSERT_EQ(0, GIT(pa, "commit", "-am", "A"));
    ASSERT_EQ(0, GIT(pb, "commit", "-am", "B"));
    ASSERT_EQ(0, task_workspace_integrate(a, err, sizeof err));
    ASSERT_EQ(0, GIT(p, "commit", "-m", "integrate A"));
    ASSERT_EQ(1, task_workspace_integrate(b, err, sizeof err));
    ASSERT_EQ(0, GIT(p, "ls-files", "--unmerged"));
    ASSERT(output.len);
    ASSERT_EQ(0, task_workspace_inspect(a, &r, err, sizeof err));
    ASSERT(strstr(r.patch, "+worker A"));
    task_workspace_result_free(&r);
    ASSERT_EQ(0, task_workspace_inspect(b, &r, err, sizeof err));
    ASSERT(strstr(r.patch, "+worker B"));
    task_workspace_result_free(&r);
    ASSERT_EQ(0, task_workspace_cleanup(a, err, sizeof err));
    ASSERT_EQ(0, task_workspace_cleanup(a, err, sizeof err));
    task_workspace_close(a);
    ASSERT_EQ(0, task_workspace_open(p, identity(0), &a, err, sizeof err));
    ASSERT_EQ(0, task_workspace_cleanup(a, err, sizeof err));
    task_workspace_close(a);
    task_workspace_close(b);
    finish(p);
    /* Real process loss stays in the process suite, not the macOS leak suite. */
    p = fixture();
    ASSERT(p);
    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        task_workspace *w = NULL;
        if (task_workspace_prepare(p, identity(0), NULL, &w, err, sizeof err)) _exit(2);
        if (write_file(task_workspace_path(w), "same.txt", "crash residue\n")) _exit(3);
        _exit(0); /* no close: OS releases lock, metadata and edits survive */
    }
    int status;
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    task_workspace *lost = NULL;
    ASSERT_EQ(0, task_workspace_open(p, identity(0), &lost, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_cleanup(lost, err, sizeof err));
    task_workspace_result residue;
    ASSERT_EQ(0, task_workspace_inspect(lost, &residue, err, sizeof err));
    ASSERT(strstr(residue.patch, "+crash residue"));
    task_workspace_result_free(&residue);
    task_workspace_close(lost);
    finish(p);
    PASS();
}

TEST task_workspace_dirty_launch_and_cancel(void) {
    char *p = fixture();
    ASSERT(p);
    task_workspace *w = NULL;
    ASSERT_EQ(0, write_file(p, "untracked", "keep me"));
    ASSERT_EQ(-1, task_workspace_prepare(p, identity(0), NULL, &w, err, sizeof err));
    ASSERT_EQ(0, task_workspace_prepare(p, identity(0), "HEAD", &w, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_integrate(w, err, sizeof err));
    ASSERT_EQ(0, write_file(task_workspace_path(w), "untracked", "worker unsaved"));
    task_workspace_close(w); /* cancellation: only release the handle */
    ASSERT_EQ(0, task_workspace_open(p, identity(0), &w, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_cleanup(w, err, sizeof err));
    task_workspace_result r;
    ASSERT_EQ(0, task_workspace_inspect(w, &r, err, sizeof err));
    ASSERT(strstr(r.status, "untracked"));
    task_workspace_result_free(&r);
    task_workspace_close(w);
    char *untracked = path_join(p, "untracked");
    ASSERT(untracked);
    ASSERT_EQ(0, unlink(untracked));
    free(untracked);
    ASSERT_EQ(0, write_file(p, "same.txt", "launch unsaved\n"));
    ASSERT_EQ(-1, task_workspace_prepare(p, identity(1), NULL, &w, err, sizeof err));
    finish(p);
    PASS();
}

TEST task_workspace_collisions_and_foreign_tree(void) {
    char *p = fixture();
    ASSERT(p);
    task_workspace *w = NULL, *other = NULL;
    char branch[128];
    snprintf(branch, sizeof branch, "tny-task/%s-0-1", run);
    ASSERT_EQ(0, GIT(p, "branch", branch));
    ASSERT_EQ(-1, task_workspace_prepare(p, identity(0), NULL, &w, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_open(p, identity(0), &w, err, sizeof err));
    ASSERT_EQ(0, task_workspace_prepare(p, identity(1), NULL, &w, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_prepare(p, identity(1), NULL, &other, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_open(p, identity(1), &other, err, sizeof err));
    char *path = xstrdup(task_workspace_path(w));
    ASSERT(path);
    ASSERT_EQ(0, GIT(p, "worktree", "remove", "--", path));
    ASSERT_EQ(0, GIT(p, "worktree", "add", "-b", "foreign", "--", path));
    ASSERT_EQ(-1, task_workspace_cleanup(w, err, sizeof err));
    task_workspace_close(w);
    ASSERT_EQ(-1, task_workspace_open(p, identity(1), &w, err, sizeof err));
    ASSERT_EQ(0, GIT(path, "symbolic-ref", "--short", "HEAD"));
    ASSERT_STR_EQ("foreign", output.data);
    free(path);
    finish(p);
    PASS();
}

TEST task_workspace_crash_and_cleanup_recovery(void) {
    char *p = fixture();
    ASSERT(p);
    task_workspace *prepared = NULL;
    ASSERT_EQ(0, task_workspace_prepare(p, identity(0), NULL, &prepared, err, sizeof err));
    ASSERT_EQ(0, write_file(task_workspace_path(prepared), "same.txt", "crash residue\n"));
    task_workspace_close(prepared);
    task_workspace *w = NULL;
    ASSERT_EQ(0, task_workspace_open(p, identity(0), &w, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_cleanup(w, err, sizeof err));
    ASSERT_EQ(0, GIT(task_workspace_path(w), "commit", "-am", "preserve crash residue"));
    /* Crash window: Git removed tree, but metadata still says ready. */
    ASSERT_EQ(0, GIT(p, "worktree", "remove", "--", task_workspace_path(w)));
    task_workspace_close(w);
    ASSERT_EQ(0, task_workspace_open(p, identity(0), &w, err, sizeof err));
    ASSERT_EQ(0, task_workspace_cleanup(w, err, sizeof err));
    ASSERT_EQ(0, task_workspace_cleanup(w, err, sizeof err));
    task_workspace_close(w);
    finish(p);
    PASS();
}

TEST task_workspace_ignored_and_large_patch_preserved(void) {
    char *p = fixture();
    ASSERT(p);
    task_workspace *w = NULL;
    ASSERT_EQ(0, task_workspace_prepare(p, identity(0), NULL, &w, err, sizeof err));
    const char *path = task_workspace_path(w);
    ASSERT_EQ(0, write_file(path, ".gitignore", "ignored\n"));
    ASSERT_EQ(0, GIT(path, "add", ".gitignore"));
    ASSERT_EQ(0, GIT(path, "commit", "-m", "ignore"));
    ASSERT_EQ(0, write_file(path, "ignored", "must survive"));
    ASSERT_EQ(-1, task_workspace_cleanup(w, err, sizeof err));
    char large[17000];
    memset(large, 'x', sizeof large - 1);
    large[sizeof large - 1] = 0;
    ASSERT_EQ(0, write_file(path, "same.txt", large));
    task_workspace_result r;
    ASSERT_EQ(-1, task_workspace_inspect(w, &r, err, sizeof err));
    ASSERT_EQ(NULL, r.patch);
    task_workspace_close(w);
    finish(p);
    PASS();
}

TEST task_workspace_path_and_metadata_collisions(void) {
    char *p = fixture();
    ASSERT(p);
    char *root = path_join(p, ".git/tny-tasks");
    ASSERT(root);
    ASSERT_EQ(0, mkdir(root, 0700));
    char name[100];
    snprintf(name, sizeof name, "%s-0-1", run);
    char *dir = path_join(root, name);
    ASSERT(dir);
    ASSERT_EQ(0, mkdir(dir, 0700));
    ASSERT_EQ(0, write_file(dir, "tree", "foreign path"));
    task_workspace *w = NULL;
    ASSERT_EQ(-1, task_workspace_prepare(p, identity(0), NULL, &w, err, sizeof err));
    ASSERT_EQ(-1, task_workspace_open(p, identity(0), &w, err, sizeof err));
    ASSERT_EQ(0, task_workspace_prepare(p, identity(1), NULL, &w, err, sizeof err));
    char *tree = xstrdup(task_workspace_path(w));
    ASSERT(tree);
    task_workspace_close(w);
    char *owner = path_join(tree, "../owner.json");
    ASSERT(owner);
    ASSERT_EQ(0, unlink(owner));
    ASSERT_EQ(0, symlink("/dev/null", owner));
    ASSERT_EQ(-1, task_workspace_open(p, identity(1), &w, err, sizeof err));
    ASSERT_EQ(0, GIT(tree, "status", "--porcelain"));
    free(owner);
    free(tree);
    free(dir);
    free(root);
    finish(p);
    PASS();
}

SUITE(task_workspace_suite) {
    RUN_TEST(task_workspace_identity_and_platform_refusal);

    RUN_TEST(task_workspace_dirty_launch_and_cancel);
    RUN_TEST(task_workspace_collisions_and_foreign_tree);
    RUN_TEST(task_workspace_crash_and_cleanup_recovery);
    RUN_TEST(task_workspace_ignored_and_large_patch_preserved);
    RUN_TEST(task_workspace_path_and_metadata_collisions);
}
/* macOS leaks --atExit deadlocks forked children. Linux runs this suite too. */
SUITE(task_workspace_process_suite) { RUN_TEST(task_workspace_two_workers_and_conflict); }
#ifdef TASK_WORKSPACE_TEST_MAIN
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(task_workspace_suite);
    RUN_SUITE(task_workspace_process_suite);
    GREATEST_MAIN_END();
}
#endif
