/* subagent.c — the native `subagent` tool (docs/features/mcp-and-skills.md,
 * docs/adr/0087).
 *
 * A child is a durable `tny ask --json` session run as a separate tny
 * process through the host process seam, never a shell. Its argv carries
 * only selectors (cwd, provider name, model, effort, permission mode); the
 * prompt arrives on stdin, and the parent's resolved API key and base URL
 * ride a private child environment named by --api-key-env / --base-url-env.
 * A secret therefore never reaches argv, a diagnostic or the parent's own
 * environment. inspect/lifecycle read the stored session and its writer
 * lock; they never start a process. */
#include "core/subagent.h"
#include "util/process.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define SUBAGENT_OUT_MAX         (8u * 1024u * 1024u) /* child --json stdout */
#define SUBAGENT_CANCEL_GRACE_MS (TNY_PROCESS_CANCEL_GRACE_MS + 1000)
#define SUBAGENT_DRAIN_MS        1000 /* stdout still held by a descendant */

typedef enum { SA_CREATE, SA_MESSAGE, SA_INSPECT, SA_LIFECYCLE, SA_NONE } sa_action;

static const char *const SA_NAMES[] = {"create", "message", "inspect", "lifecycle"};
static const char *const SA_EXAMPLES[] = {
    "{\"action\":\"create\",\"prompt\":\"...\"}",
    "{\"action\":\"message\",\"id\":\"<id from create>\",\"prompt\":\"...\"}",
    "{\"action\":\"inspect\",\"id\":\"<id from create>\"}",
    "{\"action\":\"lifecycle\",\"id\":\"<id from create>\"}",
};

/* ---- validation ---- */

static bool sa_valid_id(const char *id, size_t len) {
    if (!id || len != 16) return false;
    for (size_t i = 0; i < len; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

static sa_action sa_parse_action(yyjson_val *args, char **err) {
    *err = NULL;
    size_t len = 0;
    const char *action = yyjson_is_obj(args) ? jget_strn(args, "action", &len) : NULL;
    if (!action || !len || strlen(action) != len) {
        *err = tool_err("SUBAGENT_INVALID_ARGUMENT: action must be create, message, inspect or "
                        "lifecycle. Example: %s",
                        SA_EXAMPLES[SA_CREATE]);
        return SA_NONE;
    }
    for (int a = SA_CREATE; a < SA_NONE; a++)
        if (strcmp(action, SA_NAMES[a]) == 0) return (sa_action)a;
    *err = tool_err("SUBAGENT_UNSUPPORTED_ACTION: supported actions are create, message, inspect "
                    "and lifecycle; relationship, configure and queued messages are not "
                    "supported. Example: %s",
                    SA_EXAMPLES[SA_CREATE]);
    return SA_NONE;
}

/* Contexts where no child can be started or addressed. Hidden from the
 * advertised schema too (tools.c); this is the direct/replayed-call answer. */
static char *sa_context_error(const tools_env *env) {
    const tny_ctx *ctx = env->ctx;
    if (ctx->library_mode)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: subagent is unavailable in embedded "
                        "runtimes; do the work in this turn");
    if (ctx->prompt_optimisation)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: subagent is unavailable during prompt "
                        "optimisation");
    if (ctx->tool_profile != TNY_TOOLS_ALL)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: subagent is unavailable in the %s tool "
                        "profile; run tny ask -B --json \"...\" through terminal and read it with "
                        "tny session <id> --wait, or use TNY_TOOLS=all",
                        tny_tool_profile_name(ctx->tool_profile));
    if (ctx->ssh_host)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: subagent is unavailable with --ssh because "
                        "a child would run its tools on this machine, not the remote host; do "
                        "the work in this session");
    if (ctx->backend != TNY_BK_OPENAI)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: subagent needs tny's native "
                        "OpenAI-compatible loop; host providers run their own agents");
    return NULL;
}

char *tny_subagent_prepare_error(const tools_env *env, yyjson_val *args) {
    char *err = sa_context_error(env);
    if (err) return err;
    sa_action action = sa_parse_action(args, &err);
    if (action == SA_NONE) return err;
    if (env->ctx->no_save && action != SA_CREATE)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: ephemeral children are one-shot and store "
                        "no session, so %s has nothing to address; use %s with the complete "
                        "task, or run tny without --ephemeral",
                        SA_NAMES[action], SA_EXAMPLES[SA_CREATE]);
    size_t idx, max;
    yyjson_val *key, *value;
    if (yyjson_is_obj(args)) {
        yyjson_obj_foreach(args, idx, max, key, value) {
            (void)value;
            const char *name = yyjson_get_str(key);
            if (!name || (strcmp(name, "action") != 0 && strcmp(name, "id") != 0 &&
                          strcmp(name, "prompt") != 0))
                return tool_err("SUBAGENT_INVALID_ARGUMENT: only action, id and prompt are "
                                "accepted. Example: %s",
                                SA_EXAMPLES[action]);
        }
    }
    yyjson_val *id = jget(args, "id");
    if (action == SA_CREATE && id)
        return tool_err("SUBAGENT_INVALID_ARGUMENT: create allocates the child id; omit id. "
                        "Valid: %s, then pass the returned id to message, inspect or lifecycle",
                        SA_EXAMPLES[SA_CREATE]);
    if (action != SA_CREATE &&
        (!yyjson_is_str(id) || !sa_valid_id(yyjson_get_str(id), yyjson_get_len(id))))
        return tool_err("SUBAGENT_INVALID_ARGUMENT: %s needs the 16-character lowercase hex id "
                        "returned by create. Example: %s",
                        SA_NAMES[action], SA_EXAMPLES[action]);
    yyjson_val *prompt = jget(args, "prompt");
    bool wants_prompt = action == SA_CREATE || action == SA_MESSAGE;
    if (!wants_prompt && prompt)
        return tool_err("SUBAGENT_INVALID_ARGUMENT: %s takes no prompt. Example: %s",
                        SA_NAMES[action], SA_EXAMPLES[action]);
    if (wants_prompt) {
        const char *text = yyjson_is_str(prompt) ? yyjson_get_str(prompt) : NULL;
        size_t len = text ? yyjson_get_len(prompt) : 0;
        if (!text || !len || strlen(text) != len || !utf8_valid_bytes(text, len))
            return tool_err("SUBAGENT_INVALID_ARGUMENT: %s needs a nonempty UTF-8 prompt. "
                            "Example: %s",
                            SA_NAMES[action], SA_EXAMPLES[action]);
    }
    return NULL;
}

/* ---- launch plan: argv selectors + private environment ---- */

static bool env_named(const char *entry, const char *name) {
    size_t n = strlen(name);
    return strncmp(entry, name, n) == 0 && entry[n] == '=';
}

static int plan_assign(tny_subagent_plan *plan, const char *name, const char *value) {
    if (plan->n_owned >= (int)(sizeof plan->owned / sizeof plan->owned[0])) return -1;
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s=", name);
    buf_appends(&b, value);
    char *entry = buf_detach(&b);
    if (!entry) return -1;
    plan->owned[plan->n_owned++] = entry;
    return 0;
}

void tny_subagent_plan_free(tny_subagent_plan *plan) {
    if (!plan) return;
    free(plan->argv[0]);
    for (int i = 0; i < plan->n_owned; i++) secure_free(plan->owned[i]);
    free(plan->envp);
    memset(plan, 0, sizeof *plan);
}

int tny_subagent_plan_build(const tools_env *env, const char *resume_id, tny_subagent_plan *plan) {
    memset(plan, 0, sizeof *plan);
    const tny_ctx *ctx = env->ctx;
    char *exe = tny_process_self_path();
    if (!exe) return -1;
    bool key = ctx->api_key && *ctx->api_key;
    bool url = ctx->base_url && *ctx->base_url;
    bool token = ctx->chatgpt_token && *ctx->chatgpt_token;
    bool account = ctx->chatgpt_account_id && *ctx->chatgpt_account_id;

    int n = 0;
    char **argv = plan->argv;
    argv[n++] = exe;
    argv[n++] = (char *)"--cwd";
    argv[n++] = ctx->cwd;
    /* The resolved provider travels with the child: re-resolving from
     * settings would let a remembered host last_provider re-route it. */
    argv[n++] = (char *)"--provider";
    argv[n++] = (char *)tny_provider_name(ctx);
    if (key) {
        argv[n++] = (char *)"--api-key-env";
        argv[n++] = (char *)TNY_SUBAGENT_KEY_ENV;
    }
    if (url) {
        argv[n++] = (char *)"--base-url-env";
        argv[n++] = (char *)TNY_SUBAGENT_URL_ENV;
    }
    if (ctx->wire_api) {
        argv[n++] = (char *)"--wire-api";
        argv[n++] = (char *)(tny_wire_is_chat(ctx->wire_api) ? "chat" : "responses");
    }
    if (ctx->model) {
        argv[n++] = (char *)"--model";
        argv[n++] = ctx->model;
    }
    if (ctx->reasoning_effort && *ctx->reasoning_effort) {
        argv[n++] = (char *)"--effort";
        argv[n++] = ctx->reasoning_effort;
    }
    /* children cannot raise permission mode above the creator */
    argv[n++] = (char *)"--permission-mode";
    argv[n++] = (char *)tny_perm_mode_name(ctx->perm_mode);
    if (ctx->no_save) argv[n++] = (char *)"--ephemeral";
    argv[n++] = (char *)"ask";
    argv[n++] = (char *)"--json";
    argv[n++] = (char *)"--stdin";
    if (resume_id) {
        argv[n++] = (char *)"--resume-id";
        argv[n++] = (char *)resume_id;
    }
    argv[n] = NULL;

    /* Private carriers, then ceilings: TNY_NESTED clamps the child's settings
     * to the parent's mode and TNY_TOOLS pins its profile. An inherited
     * TNY_PERMISSION_MODE is dropped; the explicit flag above replaces it. */
    int rc = 0;
    if (key) rc |= plan_assign(plan, TNY_SUBAGENT_KEY_ENV, ctx->api_key);
    if (url) rc |= plan_assign(plan, TNY_SUBAGENT_URL_ENV, ctx->base_url);
    if (token) rc |= plan_assign(plan, "CHATGPT_ACCESS_TOKEN", ctx->chatgpt_token);
    if (account) rc |= plan_assign(plan, "CHATGPT_ACCOUNT_ID", ctx->chatgpt_account_id);
    rc |= plan_assign(plan, "TNY_NESTED", "1");
    rc |= plan_assign(plan, "TNY_NESTED_MODE", tny_perm_mode_name(ctx->perm_mode));
    rc |= plan_assign(plan, "TNY_TOOLS", tny_tool_profile_name(ctx->tool_profile));
    size_t count = 0;
    while (environ && environ[count]) count++;
    plan->envp = rc == 0 ? calloc(count + (size_t)plan->n_owned + 1, sizeof *plan->envp) : NULL;
    if (!plan->envp) {
        tny_subagent_plan_free(plan);
        return -1;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        const char *e = environ[i];
        /* A flag-selected ChatGPT token wins over the environment in the
         * parent; the child gets exactly that token and account. */
        if (env_named(e, TNY_SUBAGENT_KEY_ENV) || env_named(e, TNY_SUBAGENT_URL_ENV) ||
            env_named(e, "TNY_NESTED") || env_named(e, "TNY_NESTED_MODE") ||
            env_named(e, "TNY_TOOLS") || env_named(e, "TNY_PERMISSION_MODE") ||
            (token && env_named(e, "CHATGPT_ACCESS_TOKEN")) ||
            ((token || account) && env_named(e, "CHATGPT_ACCOUNT_ID")))
            continue;
        plan->envp[used++] = environ[i];
    }
    for (int i = 0; i < plan->n_owned; i++) plan->envp[used++] = plan->owned[i];
    plan->envp[used] = NULL;
    return 0;
}

/* ---- one child process ---- */

typedef struct {
    int spawn_error; /* errno before the child existed; 0 once it started */
    bool status_known, cancelled, truncated;
    int status; /* waitpid status when status_known */
    buf_t out;  /* at most SUBAGENT_OUT_MAX bytes of child stdout */
} sa_proc;

/* The prompt as the child's stdin: an unlinked private temp file, so a
 * large prompt needs no pipe writer and never appears on argv. */
static int sa_input_fd(const char *prompt) {
    const char *tmp = getenv("TMPDIR");
    char path[4096];
    snprintf(path, sizeof path, "%s/tny-subagent-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    size_t len = strlen(prompt);
    const char *p = prompt;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    if (lseek(fd, 0, SEEK_SET) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void sa_proc_run(tools_env *env, char *const argv[], char *const envp[], const char *prompt,
                        sa_proc *p) {
    memset(p, 0, sizeof *p);
    buf_init(&p->out);
    int in_fd = sa_input_fd(prompt);
    if (in_fd < 0) {
        p->spawn_error = EIO;
        return;
    }
    int pipes[2] = {-1, -1};
    if (pipe(pipes) != 0) {
        p->spawn_error = errno;
        close(in_fd);
        return;
    }
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = -1;
    p->spawn_error = tny_process_spawn(argv, envp, in_fd, pipes[1], &pid);
    close(in_fd);
    close(pipes[1]);
    if (p->spawn_error) {
        close(pipes[0]);
        return;
    }
    /* No env->control_pump here: the child never talks to this session's
     * socket, and the runner's pump can dispatch a cancel into the backend
     * while this call is still on its stack. Frontends also SIGTERM the
     * runner, which reaches env->cancelled through the engine's probe. */
    bool eof = false, reaped = false, killed = false;
    int64_t cancel_deadline = 0, drain_deadline = 0;
    while (!reaped || (!eof && monotonic_ms() < drain_deadline)) {
        int64_t now = monotonic_ms();
        if (!reaped && !p->cancelled && env->cancelled && env->cancelled(env->cancelled_ud)) {
            /* SIGINT is the child's own ^C: an isolated child cancels its
             * session runner, an in-process one ends the turn interrupted.
             * The tree kill below is only the fallback for a wedged child. */
            p->cancelled = true;
            kill(pid, SIGINT);
            cancel_deadline = now + SUBAGENT_CANCEL_GRACE_MS;
        }
        if (p->cancelled && !reaped && !killed && now >= cancel_deadline) {
            tny_process_kill_tree(pid);
            killed = true;
        }
        if (!reaped) {
            int status = 0;
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid || (got < 0 && errno != EINTR)) {
                reaped = true;
                p->status_known = got == pid;
                p->status = status;
                drain_deadline = now + SUBAGENT_DRAIN_MS;
            }
        }
        struct pollfd pf = {pipes[0], POLLIN, 0};
        int pr = tny_poll(eof ? NULL : &pf, eof ? 0 : 1, 100);
        if (pr < 0 && errno != EINTR) eof = true;
        if (pr <= 0 || eof) continue;
        char chunk[8192];
        ssize_t n = read(pipes[0], chunk, sizeof chunk);
        if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) eof = true;
        else if (n > 0) {
            size_t keep = (size_t)n;
            if (keep > SUBAGENT_OUT_MAX - p->out.len) {
                keep = SUBAGENT_OUT_MAX - p->out.len;
                p->truncated = true;
            }
            if (keep) buf_append(&p->out, chunk, keep);
        }
    }
    if (!eof) p->truncated = true; /* output still open after the child exited */
    close(pipes[0]);
}

/* ---- outcome ---- */

static bool sa_session_stored(tny_ctx *ctx, const char *id) {
    tny_session_state *s = id ? session_open(ctx, id) : NULL;
    session_close(s);
    return s != NULL;
}

static char *sa_success(tools_env *env, const char *sid, const char *output) {
    buf_t r;
    buf_init(&r);
    if (sid) {
        buf_appendf(&r, "subagent %s finished.\n", sid);
        buf_appendf(&r, "id: %s (use action=message id=%s to continue)\n", sid, sid);
    } else {
        buf_appends(&r, "ephemeral subagent finished; no resumable id was stored.\n");
    }
    buf_appends(&r, "result:\n");
    buf_appends(&r, output);
    char *result = r.oom ? NULL : tool_bound_result(env, r.data, r.len);
    buf_free(&r);
    return result;
}

static char *sa_outcome(tools_env *env, sa_action action, const char *resume_id, sa_proc *p) {
    if (p->spawn_error == ENOTSUP)
        return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: subagent needs a native tny build; this "
                        "build cannot start child processes");
    if (p->spawn_error)
        return tool_err("SUBAGENT_LAUNCH_FAILED: could not start a child tny process; retry, or "
                        "run tny ask --json through terminal");
    tny_ctx *ctx = env->ctx;
    bool exited0 = p->status_known && WIFEXITED(p->status) && WEXITSTATUS(p->status) == 0;
    yyjson_doc *doc = p->truncated ? NULL : jparse(p->out.data, p->out.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (root && !yyjson_is_obj(root)) root = NULL;
    const char *output = jget_str(root, "output");
    yyjson_val *code = jget(root, "exit_code");
    const char *cerr = jget_str(root, "error");
    size_t sid_len = 0;
    const char *sid = jget_strn(root, "session_id", &sid_len);
    /* Only a well-formed, stored child id is ever named back to the model. */
    const char *known = action == SA_MESSAGE ? resume_id : NULL;
    if (!known && !ctx->no_save && sa_valid_id(sid, sid_len) && sa_session_stored(ctx, sid))
        known = sid;
    bool reported = output && yyjson_is_int(code);
    bool reported_ok = reported && yyjson_get_int(code) == 0 && !(cerr && *cerr);
    char *result = NULL;
    if (exited0 && reported_ok) {
        if (ctx->no_save) result = sa_success(env, NULL, output);
        else if (known && sid && strcmp(known, sid) == 0) result = sa_success(env, known, output);
    }
    if (!result && p->cancelled) {
        result = known ? tool_err("SUBAGENT_CANCELLED: the turn was cancelled and child %s was "
                                  "stopped; check {\"action\":\"lifecycle\",\"id\":\"%s\"} before "
                                  "continuing",
                                  known, known)
                       : tool_err("SUBAGENT_CANCELLED: the turn was cancelled and the child "
                                  "process was stopped before it reported a session");
    } else if (!result && p->status_known && !exited0 && !root && action == SA_MESSAGE &&
               session_is_running(ctx, resume_id)) {
        /* lost the lock race after the pre-launch probe: nothing changed */
        result = tool_err("SUBAGENT_SESSION_BUSY: that child is running a turn; check "
                          "{\"action\":\"lifecycle\",\"id\":\"%s\"} and retry after it finishes",
                          resume_id);
    } else if (!result && p->status_known && (!exited0 || (reported && !reported_ok))) {
        /* the process status, else the turn's own reported failure */
        char how[48];
        if (WIFSIGNALED(p->status)) snprintf(how, sizeof how, "signal %d", WTERMSIG(p->status));
        else if (!exited0) snprintf(how, sizeof how, "exit %d", WEXITSTATUS(p->status));
        else if (yyjson_get_int(code) != 0)
            snprintf(how, sizeof how, "exit %d", (int)yyjson_get_int(code));
        else snprintf(how, sizeof how, "reported an error");
        result = known ? tool_err("SUBAGENT_CHILD_FAILED: child %s failed (%s); its session keeps "
                                  "the details. Check {\"action\":\"lifecycle\",\"id\":\"%s\"} "
                                  "and retry with action=message",
                                  known, how, known)
                       : tool_err("SUBAGENT_CHILD_FAILED: the child failed (%s) before storing a "
                                  "session; check the provider setup with tny doctor, then retry "
                                  "create",
                                  how);
    } else if (!result) {
        result = tool_err("SUBAGENT_INVALID_RESPONSE: the child exited without a complete turn "
                          "result; check tny sessions for its state before retrying");
    }
    yyjson_doc_free(doc);
    return result;
}

char *tny_subagent_run(tools_env *env, const char *action, const char *resume_id,
                       char *const argv[], char *const envp[], const char *prompt) {
    sa_proc p;
    sa_proc_run(env, argv, envp, prompt, &p);
    char *result = sa_outcome(
        env, action && strcmp(action, "message") == 0 ? SA_MESSAGE : SA_CREATE, resume_id, &p);
    buf_free(&p.out);
    return result;
}

/* ---- stored state: inspect / lifecycle ---- */

static const char *sa_state(tny_ctx *ctx, tny_session_state *s, bool *running, bool *has_exit,
                            int64_t *exit_code) {
    *running = session_is_running(ctx, s->id);
    yyjson_mut_val *ec = yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "exit_code");
    *has_exit = !*running && ec && yyjson_mut_is_int(ec);
    *exit_code = *has_exit ? yyjson_mut_get_int(ec) : 0;
    const char *st = session_status(s);
    if (*running) return "running";
    if (!st) return "unknown"; /* never recorded one: not a success */
    if (strcmp(st, "running") == 0) {
        *has_exit = false;
        return "stale"; /* stored running without a live writer */
    }
    if (strcmp(st, "done") == 0 || strcmp(st, "error") == 0 || strcmp(st, "interrupted") == 0)
        return st;
    return "unknown";
}

/* key: value, with control bytes of stored text flattened to spaces */
static void sa_line(buf_t *b, const char *key, const char *value) {
    buf_appendf(b, "%s: ", key);
    for (const char *c = value; *c; c++) {
        char ch = (unsigned char)*c < 0x20 ? ' ' : *c;
        buf_append(b, &ch, 1);
    }
    buf_appends(b, "\n");
}

static const char *sa_meta(tny_session_state *s, const char *key) {
    const char *v = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), key));
    return v && *v ? v : "unknown";
}

static char *sa_describe(tools_env *env, sa_action action, tny_session_state *s) {
    bool running, has_exit;
    int64_t exit_code;
    const char *state = sa_state(env->ctx, s, &running, &has_exit, &exit_code);
    buf_t r;
    buf_init(&r);
    buf_appendf(&r, "subagent %s\n", s->id);
    if (action == SA_INSPECT) {
        const char *title = session_title(s);
        sa_line(&r, "title", title && *title ? title : "(none)");
        buf_appendf(&r, "turns: %d\n", session_turns(s));
        sa_line(&r, "provider", sa_meta(s, "backend"));
        sa_line(&r, "model", sa_meta(s, "model"));
        sa_line(&r, "created", sa_meta(s, "created"));
        sa_line(&r, "updated", sa_meta(s, "updated"));
    }
    buf_appendf(&r, "status: %s\n", state);
    if (has_exit) buf_appendf(&r, "exit_code: %lld\n", (long long)exit_code);
    else buf_appends(&r, "exit_code: null\n");
    buf_appendf(&r, "running: %s\n", running ? "true" : "false");
    buf_appendf(&r, "resumable: %s", !running && !session_host_pointer(s) ? "true" : "false");
    yyjson_mut_val *stored = yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "result");
    const char *out = yyjson_mut_get_str(yyjson_mut_obj_get(stored, "output"));
    if (action == SA_INSPECT && strcmp(state, "done") == 0 && out) {
        buf_appends(&r, "\nresult:\n");
        buf_appends(&r, out);
    }
    char *result = r.oom ? NULL : tool_bound_result(env, r.data, r.len);
    buf_free(&r);
    return result;
}

/* ---- entry ---- */

char *tny_subagent_execute(tools_env *env, yyjson_val *args) {
    char *err = tny_subagent_prepare_error(env, args);
    if (err) return err;
    sa_action action = sa_parse_action(args, &err);
    if (action == SA_NONE) return err; /* not reached: validated above */
    tny_ctx *ctx = env->ctx;
    const char *id = action == SA_CREATE ? NULL : jget_str(args, "id");
    tny_session_state *child = NULL;
    if (id) {
        if (action == SA_MESSAGE && env->session && env->session->id &&
            strcmp(id, env->session->id) == 0)
            return tool_err("SUBAGENT_SESSION_BUSY: that id is this parent session, which is "
                            "running this turn; message only ids returned by create");
        child = session_open(ctx, id);
        if (!child)
            return tool_err("SUBAGENT_SESSION_NOT_FOUND: no stored child session has that id in "
                            "this workspace; create one with %s and use the id it returns",
                            SA_EXAMPLES[SA_CREATE]);
        if (action == SA_INSPECT || action == SA_LIFECYCLE) {
            char *described = sa_describe(env, action, child);
            session_close(child);
            return described;
        }
        bool host_owned = session_host_pointer(child) != NULL;
        session_close(child);
        if (host_owned)
            return tool_err("SUBAGENT_UNSUPPORTED_CONTEXT: that session belongs to a host "
                            "provider; message continues only native subagent sessions");
        if (session_is_running(ctx, id))
            return tool_err("SUBAGENT_SESSION_BUSY: that child is running a turn; check "
                            "{\"action\":\"lifecycle\",\"id\":\"%s\"} and retry after it "
                            "finishes",
                            id);
    }
    if (!(ctx->api_key && *ctx->api_key) &&
        !str_starts(ctx->base_url ? ctx->base_url : "", "http://"))
        return tool_err("SUBAGENT_AUTH_UNAVAILABLE: the parent provider has no resolved "
                        "credential to hand a child; configure its key (for example "
                        "--api-key-env NAME, tny login or tny provider setup) and retry");
    tny_subagent_plan plan;
    if (tny_subagent_plan_build(env, id, &plan) != 0) {
        sa_proc failed = {.spawn_error = errno == ENOTSUP ? ENOTSUP : ENOENT};
        return sa_outcome(env, action, id, &failed);
    }
    char *result =
        tny_subagent_run(env, SA_NAMES[action], id, plan.argv, plan.envp, jget_str(args, "prompt"));
    tny_subagent_plan_free(&plan);
    return result;
}
