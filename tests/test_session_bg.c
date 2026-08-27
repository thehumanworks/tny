/* test_session_bg.c — background-ask session plumbing (docs/adr/0031):
 * status/result round-trips, legacy-session absence, the flock writer lock
 * (cross-process via fork), and the pid control file. */
#include "greatest.h"
#include "core/config.h"
#include "core/session.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef struct {
    char root[512];
    char workspace[540];
    char *old_home;
} bg_env;

static bool bg_chmod_denies_owner_writes(void) {
#if defined(__CYGWIN__) || defined(__MSYS__)
    /* Their chmod mode bits on Windows do not reliably make an NTFS
     * directory unwritable to its owner. */
    return false;
#else
    return geteuid() != 0; /* root can rewrite chmod(0500) directories */
#endif
}

static void bg_env_begin(bg_env *e) {
    memset(e, 0, sizeof *e);
    const char *old = getenv("HOME");
    if (old) e->old_home = xstrdup(old);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(e->root, sizeof e->root, "%s/tny-bg-test-XXXXXX", tmp);
    if (!mkdtemp(e->root)) abort();
    setenv("HOME", e->root, 1);
    snprintf(e->workspace, sizeof e->workspace, "%s/workspace", e->root);
    if (mkdir_p(e->workspace) != 0) abort();
}

static void bg_env_end(bg_env *e) {
    if (e->old_home) setenv("HOME", e->old_home, 1);
    else unsetenv("HOME");
    free(e->old_home);
}

TEST bg_status_running_then_done_roundtrip(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);

    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    session_set_status_running(s);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT(session_status(s));
    ASSERT_STR_EQ("running", session_status(s));
    session_set_status_finished(s, "done", 0, "{\"output\":\"hi\"}");
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_STR_EQ("done", session_status(s));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
    yyjson_mut_val *ec = yyjson_mut_obj_get(root, "exit_code");
    ASSERT(ec);
    ASSERT_EQ(0, (int)yyjson_mut_get_int(ec));
    yyjson_mut_val *result = yyjson_mut_obj_get(root, "result");
    ASSERT(result);
    const char *out = yyjson_mut_get_str(yyjson_mut_obj_get(result, "output"));
    ASSERT(out);
    ASSERT_STR_EQ("hi", out);
    session_close(s);

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* A result that does not parse must be dropped, not corrupt the doc. */
TEST bg_bad_result_json_stores_nothing(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    session_set_status_finished(s, "error", 2, "{not json");
    ASSERT_STR_EQ("error", session_status(s));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
    ASSERT_EQ(NULL, (void *)yyjson_mut_obj_get(root, "result"));
    ASSERT_EQ(0, session_save(s));
    session_close(s);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* Legacy sessions lack every background field: nothing crashes, listing
 * still works, and readers see "not a background task". */
TEST bg_legacy_session_has_no_status(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);

    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    session_add_text(s, "user", "hello");
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_EQ(NULL, (void *)session_status(s));
    session_close(s);
    ASSERT_FALSE(session_is_running(ctx, id));
    ASSERT_EQ(-1, (int)session_read_pid(ctx, id));

    int n = 0;
    session_meta *m = session_list(ctx, false, 10, NULL, &n);
    ASSERT_EQ(1, n);
    session_meta_free(m, n);

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* flock semantics are per open-file-description, so contention is only
 * observable cross-process: a fork()ed child takes the lock, the parent
 * probe sees "running"; the child exits (self-release), the probe clears. */
TEST bg_lock_contention_across_processes(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);

    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    int to_child[2], to_parent[2];
    ASSERT_EQ(0, pipe(to_child));
    ASSERT_EQ(0, pipe(to_parent));
    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* child: acquire, signal the parent, wait for release order, exit
         * without releasing (the exit itself must free the flock) */
        close(to_child[1]);
        close(to_parent[0]);
        tny_session_state *cs = session_open(ctx, id);
        char ok = cs && session_lock_acquire(cs) == 0 ? '1' : '0';
        if (write(to_parent[1], &ok, 1) != 1) _exit(2);
        char go;
        if (read(to_child[0], &go, 1) != 1) _exit(2);
        _exit(ok == '1' ? 0 : 1);
    }
    close(to_child[0]);
    close(to_parent[1]);
    char ok = 0;
    ASSERT_EQ(1, (int)read(to_parent[0], &ok, 1));
    ASSERT_EQ('1', ok);
    ASSERT(session_is_running(ctx, id)); /* held by the child */

    /* a second writer must fail to acquire while the child holds it */
    tny_session_state *w = session_open(ctx, id);
    ASSERT(w);
    ASSERT_EQ(-1, session_lock_acquire(w));
    session_close(w);

    ASSERT_EQ(1, (int)write(to_child[1], "g", 1));
    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(to_child[1]);
    close(to_parent[0]);

    ASSERT_FALSE(session_is_running(ctx, id)); /* self-released on exit */
    w = session_open(ctx, id);
    ASSERT(w);
    ASSERT_EQ(0, session_lock_acquire(w)); /* free again */
    ASSERT_EQ(0, session_lock_acquire(w)); /* idempotent re-acquire */
    session_lock_release(w);
    session_close(w);

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

TEST bg_pid_file_roundtrip(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);

    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(-1, (int)session_read_pid(ctx, id)); /* absent */
    ASSERT_EQ(0, session_write_pid(s, getpid()));
    ASSERT_EQ((int)getpid(), (int)session_read_pid(ctx, id));

    /* malformed pid files read as -1: garbage, trailing junk after the
     * digits, and a zero pid (never a valid signal target). CRLF is the
     * one tolerated terminator beyond \n. */
    char *file = path_join(s->dir, "pid");
    ASSERT_EQ(0, file_write_atomic(file, "notapid\n", 8));
    ASSERT_EQ(-1, (int)session_read_pid(ctx, id));
    ASSERT_EQ(0, file_write_atomic(file, "123x", 4));
    ASSERT_EQ(-1, (int)session_read_pid(ctx, id));
    ASSERT_EQ(0, file_write_atomic(file, "0\n", 2));
    ASSERT_EQ(-1, (int)session_read_pid(ctx, id));
    ASSERT_EQ(0, file_write_atomic(file, "42\r\n", 4));
    ASSERT_EQ(42, (int)session_read_pid(ctx, id));
    free(file);

    session_close(s);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* NULL / ephemeral API contract: every background-session entry point must
 * no-op (or refuse) on a NULL state and on ephemeral contexts — never
 * dereference, never pretend success where the ADR says refuse. */
TEST bg_api_null_and_ephemeral_contract(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);

    session_set_status_running(NULL); /* no-op, no crash */
    session_set_status_finished(NULL, "done", 0, NULL);
    session_lock_release(NULL);
    ASSERT_EQ(-1, session_write_pid(NULL, 1));
    ASSERT_EQ(-1, (int)session_read_pid(NULL, "0123456789abcdef"));
    ASSERT_EQ(-1, (int)session_read_pid(ctx, NULL));
    ASSERT_FALSE(session_is_running(NULL, "0123456789abcdef"));
    ASSERT_FALSE(session_is_running(ctx, NULL));

    /* a NULL status must not clobber the stored one */
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    session_set_status_running(s);
    session_set_status_finished(s, NULL, 7, NULL);
    ASSERT_STR_EQ("running", session_status(s));

    /* ephemeral: pid writes refuse, liveness is always false */
    ctx->no_save = true;
    ASSERT_EQ(-1, session_write_pid(s, getpid()));
    ASSERT_FALSE(session_is_running(ctx, s->id));
    ctx->no_save = false;

    session_close(s);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* Filesystem failure is an error, not a silent success: an uncreatable
 * session dir and an unwritable one must fail lock/pid acquisition. */
TEST bg_lock_and_pid_report_fs_failure(void) {
    if (!bg_chmod_denies_owner_writes()) SKIP();
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx); /* dir not created yet */
    ASSERT(s);

    char parent[600];
    snprintf(parent, sizeof parent, "%s", s->dir);
    char *slash = strrchr(parent, '/');
    ASSERT(slash);
    *slash = 0;
    ASSERT_EQ(0, mkdir_p(parent));

    ASSERT_EQ(0, chmod(parent, 0500)); /* session dir cannot be created */
    ASSERT_EQ(-1, session_lock_acquire(s));
    ASSERT_EQ(-1, session_write_pid(s, getpid())); /* mkdir fails here too */
    ASSERT_EQ(0, chmod(parent, 0700));

    ASSERT_EQ(0, mkdir_p(s->dir));
    ASSERT_EQ(0, chmod(s->dir, 0500)); /* lock/pid files cannot be created */
    ASSERT_EQ(-1, session_lock_acquire(s));
    ASSERT_EQ(-1, session_write_pid(s, getpid()));
    ASSERT_EQ(0, chmod(s->dir, 0700));

    session_close(s);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* The lock is a no-op for ephemeral runs (nothing on disk to guard) and for
 * a NULL state: both must report success, not a refusal. */
TEST bg_lock_acquire_noop_for_ephemeral(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    ctx->no_save = true;
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    ASSERT_EQ(0, session_lock_acquire(s));
    ASSERT_EQ(-1, s->lock_fd); /* nothing actually taken */
    session_close(s);
    ASSERT_EQ(0, session_lock_acquire(NULL));
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* Fork a holder that mimics the real -B child: setsid group leader owning
 * the exclusive flock, pid file written unless write_pid is false (the
 * milliseconds-wide "starting" window, ADR decision 4). ignore_term installs
 * SIG_IGN for SIGTERM before signaling readiness. Returns the child pid; the
 * parent has consumed the ready byte when this returns. */
static pid_t spawn_lock_holder_opt(tny_ctx *ctx, const char *id, bool ignore_term, bool write_pid) {
    int to_parent[2];
    if (pipe(to_parent) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(to_parent[0]);
        if (ignore_term) signal(SIGTERM, SIG_IGN);
        setsid(); /* group leader, like the real background child */
        tny_session_state *cs = session_open(ctx, id);
        if (!cs || session_lock_acquire(cs) != 0) _exit(2);
        if (write_pid && session_write_pid(cs, getpid()) != 0) _exit(2);
        if (write(to_parent[1], "r", 1) != 1) _exit(2);
        for (;;) pause(); /* until a signal kills us */
    }
    close(to_parent[1]);
    char r = 0;
    ssize_t n = read(to_parent[0], &r, 1);
    close(to_parent[0]);
    if (n != 1 || r != 'r') {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

static pid_t spawn_lock_holder(tny_ctx *ctx, const char *id, bool ignore_term) {
    return spawn_lock_holder_opt(ctx, id, ignore_term, true);
}

/* Lock held but no pid file (the "starting" window): stop cannot signal
 * anything — a distinct error, never a false "stopped". */
TEST bg_stop_without_pid_file_errors(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    pid_t pid = spawn_lock_holder_opt(ctx, id, true, false);
    ASSERT(pid > 0);
    ASSERT(session_is_running(ctx, id));

    char err[256];
    err[0] = 0;
    ASSERT_EQ(-1, session_stop(ctx, id, false, err, sizeof err));
    ASSERT(strstr(err, "no pid file"));
    ASSERT(session_is_running(ctx, id)); /* holder untouched */

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* stop --kill on an unwritable session dir: the SIGKILL lands but the
 * terminal status cannot be recorded — that is an error, not a success. */
TEST bg_stop_kill_unwritable_status_errors(void) {
    if (!bg_chmod_denies_owner_writes()) SKIP();
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    char dir[600];
    snprintf(id, sizeof id, "%s", s->id);
    snprintf(dir, sizeof dir, "%s", s->dir);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    pid_t pid = spawn_lock_holder(ctx, id, true); /* ignores SIGTERM */
    ASSERT(pid > 0);
    setenv("TNY_STOP_TIMEOUT_MS", "300", 1);
    char err[256];
    err[0] = 0;
    ASSERT_EQ(0, chmod(dir, 0500)); /* session.json rewrite must fail */
    int rc = session_stop(ctx, id, true, err, sizeof err);
    chmod(dir, 0700);
    unsetenv("TNY_STOP_TIMEOUT_MS");
    ASSERT_EQ(-1, rc);
    ASSERT(strstr(err, "cannot write terminal status"));

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* session_stop on a session nothing holds: distinct "was not running". */
TEST bg_stop_not_running_is_noop(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    char err[256];
    ASSERT_EQ(1, session_stop(ctx, id, false, err, sizeof err));
    ASSERT_EQ(1, session_stop(ctx, id, true, err, sizeof err)); /* --kill too */

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* SIGTERM path: the holder dies on the group signal, the lock self-frees,
 * session_stop reports 0. */
TEST bg_stop_sigterm_terminates_holder(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    pid_t pid = spawn_lock_holder(ctx, id, false);
    ASSERT(pid > 0);
    ASSERT(session_is_running(ctx, id));

    setenv("TNY_STOP_TIMEOUT_MS", "3000", 1);
    char err[256];
    int rc = session_stop(ctx, id, false, err, sizeof err);
    unsetenv("TNY_STOP_TIMEOUT_MS");
    ASSERT_EQ(0, rc);

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    ASSERT_FALSE(session_is_running(ctx, id));

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* With TNY_STOP_TIMEOUT_MS unset the default bounded wait applies; a holder
 * that dies on SIGTERM stops well inside it. */
TEST bg_stop_default_timeout_env_unset(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    pid_t pid = spawn_lock_holder(ctx, id, false);
    ASSERT(pid > 0);
    unsetenv("TNY_STOP_TIMEOUT_MS");
    char err[256];
    ASSERT_EQ(0, session_stop(ctx, id, false, err, sizeof err));
    waitpid(pid, NULL, 0);
    ASSERT_FALSE(session_is_running(ctx, id));

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* A pause()d process in its own group that ignores SIGTERM but holds NO
 * lock: a signal target whose death cannot free the flock. */
static pid_t spawn_plain_pause_child(void) {
    int to_parent[2];
    if (pipe(to_parent) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(to_parent[0]);
        signal(SIGTERM, SIG_IGN);
        setsid();
        if (write(to_parent[1], "r", 1) != 1) _exit(2);
        for (;;) pause();
    }
    close(to_parent[1]);
    char r = 0;
    ssize_t n = read(to_parent[0], &r, 1);
    close(to_parent[0]);
    if (n != 1 || r != 'r') {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

/* stop --kill where SIGKILLing the pid'd group does NOT free the flock
 * (a foreign writer holds it): the post-kill wait must report failure,
 * never a false "stopped". */
TEST bg_stop_kill_foreign_lock_survives_errors(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    ASSERT_EQ(0, session_lock_acquire(s)); /* WE hold the lock */

    pid_t pid = spawn_plain_pause_child(); /* pid file points elsewhere */
    ASSERT(pid > 0);
    ASSERT_EQ(0, session_write_pid(s, pid));

    setenv("TNY_STOP_TIMEOUT_MS", "200", 1);
    char err[256];
    err[0] = 0;
    int rc = session_stop(ctx, id, true, err, sizeof err);
    unsetenv("TNY_STOP_TIMEOUT_MS");
    ASSERT_EQ(-1, rc);
    ASSERT(strstr(err, "did not release its lock"));

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0)); /* the SIGKILL still landed */
    ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    session_lock_release(s);
    session_close(s);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* stop --kill where session.json vanished mid-run: the terminal status
 * cannot be recorded — a distinct error after the kill. */
TEST bg_stop_kill_unopenable_session_errors(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    char json_path[640];
    snprintf(id, sizeof id, "%s", s->id);
    snprintf(json_path, sizeof json_path, "%s/session.json", s->dir);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    pid_t pid = spawn_lock_holder(ctx, id, true); /* ignores SIGTERM */
    ASSERT(pid > 0);
    ASSERT_EQ(0, unlink(json_path)); /* liveness is the lock, not the doc */
    ASSERT(session_is_running(ctx, id));

    setenv("TNY_STOP_TIMEOUT_MS", "200", 1);
    char err[256];
    err[0] = 0;
    int rc = session_stop(ctx, id, true, err, sizeof err);
    unsetenv("TNY_STOP_TIMEOUT_MS");
    ASSERT_EQ(-1, rc);
    ASSERT(strstr(err, "cannot open after kill"));

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

/* A SIGTERM-ignoring holder: no --kill times out (2); --kill SIGKILLs the
 * group and writes the terminal status on the child's behalf. */
TEST bg_stop_force_kill_writes_terminal_status(void) {
    bg_env e;
    bg_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    char id[17];
    snprintf(id, sizeof id, "%s", s->id);
    ASSERT_EQ(0, session_save(s));
    session_close(s);

    pid_t pid = spawn_lock_holder(ctx, id, true);
    ASSERT(pid > 0);
    ASSERT(session_is_running(ctx, id));

    setenv("TNY_STOP_TIMEOUT_MS", "300", 1);
    char err[256];
    ASSERT_EQ(2, session_stop(ctx, id, false, err, sizeof err)); /* wedged */
    ASSERT(session_is_running(ctx, id)); /* steer/stop without --kill leave it */
    ASSERT_EQ(0, session_stop(ctx, id, true, err, sizeof err));
    unsetenv("TNY_STOP_TIMEOUT_MS");

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    ASSERT_FALSE(session_is_running(ctx, id));

    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT(session_status(s));
    ASSERT_STR_EQ("interrupted", session_status(s));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
    yyjson_mut_val *ec = yyjson_mut_obj_get(root, "exit_code");
    ASSERT(ec);
    ASSERT_EQ(137, (int)yyjson_mut_get_int(ec));
    session_close(s);

    tny_ctx_free(ctx);
    bg_env_end(&e);
    PASS();
}

SUITE(session_bg_suite) {
    RUN_TEST(bg_status_running_then_done_roundtrip);
    RUN_TEST(bg_bad_result_json_stores_nothing);
    RUN_TEST(bg_legacy_session_has_no_status);
    RUN_TEST(bg_lock_contention_across_processes);
    RUN_TEST(bg_pid_file_roundtrip);
    RUN_TEST(bg_api_null_and_ephemeral_contract);
    RUN_TEST(bg_lock_and_pid_report_fs_failure);
    RUN_TEST(bg_lock_acquire_noop_for_ephemeral);
    RUN_TEST(bg_stop_default_timeout_env_unset);
    RUN_TEST(bg_stop_kill_foreign_lock_survives_errors);
    RUN_TEST(bg_stop_kill_unopenable_session_errors);
    RUN_TEST(bg_stop_without_pid_file_errors);
    RUN_TEST(bg_stop_kill_unwritable_status_errors);
    RUN_TEST(bg_stop_not_running_is_noop);
    RUN_TEST(bg_stop_sigterm_terminates_holder);
    RUN_TEST(bg_stop_force_kill_writes_terminal_status);
}
