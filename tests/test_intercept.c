/* test_intercept.c — the in-process intercept of first-party tny verbs typed
 * into the terminal tool (issue #99, docs/adr/0063): the shell-word
 * recogniser, the decision table, permission parity with the typed tools,
 * and the executed results. */
#include "greatest.h"
#include "core/config.h"
#include "core/intercept.h"
#include "core/shellwords.h"
#include "core/tools.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[512], g_ws[600];

static void ensure_env(void) {
    if (g_home[0]) return;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_home, sizeof g_home, "%s%stny-intercept-XXXXXX", tmp,
             tmp[strlen(tmp) - 1] == '/' ? "" : "/");
    if (!mkdtemp(g_home)) abort();
    setenv("HOME", g_home, 1);
    unsetenv("TNY_PERMISSION_MODE");
    unsetenv("TNY_NESTED");
    unsetenv("TNY_NESTED_MODE");
    snprintf(g_ws, sizeof g_ws, "%s/ws", g_home);
    mkdir_p(g_ws);
}

static void write_workspace_file(const char *name, const char *body) {
    char *path = path_join(g_ws, name);
    if (!path || file_write_atomic(path, body, strlen(body)) != 0) abort();
    free(path);
}

static char *read_workspace_file(const char *name) {
    char *path = path_join(g_ws, name);
    char *data = path ? file_slurp(path, NULL) : NULL;
    free(path);
    if (!data) abort();
    return data;
}

static void write_settings(const char *json) {
    char *dir = path_join(g_home, ".tny");
    char *path = dir ? path_join(dir, "settings.json") : NULL;
    if (!path) abort();
    mkdir_p(dir);
    file_write_atomic(path, json, strlen(json));
    free(dir);
    free(path);
}

typedef struct {
    tny_ctx *ctx;
    perm_engine *perm;
    tny_session_state *session;
    tools_env env;
} fixture;

static void fixture_open(fixture *f, tny_perm_mode mode) {
    ensure_env();
    f->ctx = tny_ctx_load(g_ws);
    if (!f->ctx) abort();
    f->ctx->perm_mode = mode;
    f->perm = perm_new(f->ctx);
    f->session = session_new(f->ctx);
    memset(&f->env, 0, sizeof f->env);
    f->env.ctx = f->ctx;
    f->env.perm = f->perm;
    f->env.session = f->session;
}

static void fixture_close(fixture *f) {
    /* the turn's own cleanup for anything read_image / `tny image attach`
     * queued, so the fixture frees exactly what the loop would */
    char flush_error[256];
    tools_flush_images(&f->env, flush_error, sizeof flush_error);
    session_close(f->session);
    perm_free(f->perm);
    tny_ctx_free(f->ctx);
}

static char *run_terminal(fixture *f, const char *command) {
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"command\":");
    jescape(&args, command);
    buf_appends(&args, "}");
    char *result = tools_execute(&f->env, "terminal", args.data);
    buf_free(&args);
    if (!result) abort();
    return result;
}

/* ---- the shell-word recogniser ---- */

TEST shellwords_splits_quoting_like_sh(void) {
    tny_words w;
    ASSERT_EQ(0, tny_shellwords("tny edit 'two words' \"a b\" c\\ d", &w));
    ASSERT_EQ(5, w.argc);
    ASSERT_STR_EQ("tny", w.argv[0]);
    ASSERT_STR_EQ("edit", w.argv[1]);
    ASSERT_STR_EQ("two words", w.argv[2]);
    ASSERT_STR_EQ("a b", w.argv[3]);
    ASSERT_STR_EQ("c d", w.argv[4]);
    ASSERT(!w.quoted[1] && w.quoted[2] && w.quoted[3] && w.quoted[4]);
    ASSERT_EQ(0, w.stop);
    ASSERT_EQ(NULL, w.argv[w.argc]);
    tny_shellwords_free(&w);

    /* the four escapes the shell honours inside double quotes stay literal */
    ASSERT_EQ(0, tny_shellwords("tny edit \"say \\\"hi\\\"\"", &w));
    ASSERT_EQ(3, w.argc);
    ASSERT_STR_EQ("say \"hi\"", w.argv[2]);
    tny_shellwords_free(&w);
    ASSERT_EQ(0, tny_shellwords("tny edit \"back\\\\slash\"", &w));
    ASSERT_STR_EQ("back\\slash", w.argv[2]);
    tny_shellwords_free(&w);
    ASSERT_EQ(0, tny_shellwords("tny edit \"cost \\$5\"", &w));
    ASSERT_STR_EQ("cost $5", w.argv[2]);
    tny_shellwords_free(&w);
    ASSERT_EQ(0, tny_shellwords("tny edit \"tick \\`x\"", &w));
    ASSERT_STR_EQ("tick `x", w.argv[2]);
    tny_shellwords_free(&w);

    /* characters the shell passes through are not operators */
    ASSERT_EQ(0, tny_shellwords("tny edit a=b.txt", &w));
    ASSERT_EQ(3, w.argc);
    ASSERT_STR_EQ("a=b.txt", w.argv[2]);
    ASSERT_EQ(0, w.stop);
    tny_shellwords_free(&w);
    PASS();
}

TEST shellwords_stops_at_every_shell_operator(void) {
    static const char *cases[] = {"a; b",  "a | b", "a && b", "a > f",   "a < f",
                                  "a & ",  "a `b`", "a $(b)", "a $HOME", "a\nb",
                                  "a *.c", "a f?g", "a [ab]", "a {x,y}", NULL};
    for (int i = 0; cases[i]; i++) {
        tny_words w;
        ASSERT_EQ(0, tny_shellwords(cases[i], &w));
        ASSERTm(cases[i], w.stop != 0);
        ASSERT_STR_EQ("a", w.argv[0]);
        tny_shellwords_free(&w);
    }
    PASS();
}

TEST shellwords_refuses_what_it_cannot_reproduce(void) {
    tny_words w;
    ASSERT_EQ(-1, tny_shellwords("tny edit 'unterminated", &w));
    ASSERT_EQ(-1, tny_shellwords("tny edit \"unterminated", &w));
    ASSERT_EQ(-1, tny_shellwords("tny edit trailing\\", &w));
    ASSERT_EQ(-1, tny_shellwords("tny edit \"$HOME/x\"", &w));
    ASSERT_EQ(-1, tny_shellwords("tny edit \"`id`\"", &w));
    ASSERT_EQ(-1, tny_shellwords(NULL, &w));
    buf_t many;
    buf_init(&many);
    for (int i = 0; i < TNY_WORDS_MAX + 1; i++) buf_appends(&many, "w ");
    ASSERT_EQ(-1, tny_shellwords(many.data, &w));
    buf_free(&many);
    PASS();
}

TEST shellwords_expands_only_the_tilde_form_tny_resolves(void) {
    tny_words w;
    ASSERT_EQ(0, tny_shellwords("tny edit ~/notes.txt", &w));
    ASSERT_EQ(3, w.argc);
    ASSERT_STR_EQ("~/notes.txt", w.argv[2]);
    ASSERT_EQ(0, w.stop);
    tny_shellwords_free(&w);
    ASSERT_EQ(0, tny_shellwords("tny edit ~alice/notes.txt", &w));
    ASSERT_EQ(2, w.argc);
    ASSERT_EQ('~', w.stop);
    tny_shellwords_free(&w);
    /* a tilde that the shell would not expand either stays in the word */
    ASSERT_EQ(0, tny_shellwords("tny edit notes~.txt", &w));
    ASSERT_EQ(3, w.argc);
    ASSERT_STR_EQ("notes~.txt", w.argv[2]);
    tny_shellwords_free(&w);
    PASS();
}

/* ---- the decision table ---- */

static tny_intercept_kind classify(fixture *f, const char *command) {
    tny_intercept *ic = tny_intercept_parse(&f->env, command);
    tny_intercept_kind kind = ic ? ic->kind : 0;
    tny_intercept_free(ic);
    return kind;
}

TEST decision_table_intercepts_only_documented_shapes(void) {
    fixture f;
    fixture_open(&f, TNY_MODE_YOLO);

    /* recognised */
    ASSERT_EQ(TNY_INTERCEPT_EDIT, classify(&f, "tny edit notes.txt"));
    ASSERT_EQ(TNY_INTERCEPT_EDIT, classify(&f, "tny edit a=b.txt"));
    ASSERT_EQ(TNY_INTERCEPT_EDIT, classify(&f, "./tny edit notes.txt"));
    ASSERT_EQ(TNY_INTERCEPT_EDIT, classify(&f, "/usr/local/bin/tny --json edit notes.txt"));
    ASSERT_EQ(TNY_INTERCEPT_MCP_CALL, classify(&f, "tny mcp call srv/deploy"));
    ASSERT_EQ(TNY_INTERCEPT_MEMORY, classify(&f, "tny memory list"));
    ASSERT_EQ(TNY_INTERCEPT_MEMORY, classify(&f, "tny memory get k"));
    ASSERT_EQ(TNY_INTERCEPT_MEMORY, classify(&f, "tny memory set k v"));
    ASSERT_EQ(TNY_INTERCEPT_SKILL, classify(&f, "tny skill show demo"));
    ASSERT_EQ(TNY_INTERCEPT_IMAGE_ATTACH, classify(&f, "tny image attach shot.png"));
    ASSERT_EQ(TNY_INTERCEPT_ASK_USER, classify(&f, "tny ask-user 'which branch?'"));
    ASSERT_EQ(TNY_INTERCEPT_REFUSED, classify(&f, "tny ask 'do the thing'"));

    /* left to the shell */
    ASSERT_EQ(0, classify(&f, "tny ask -B 'do the thing'"));
    ASSERT_EQ(0, classify(&f, "tny ask --background 'do the thing'"));
    ASSERT_EQ(0, classify(&f, "tny sessions"));
    ASSERT_EQ(0, classify(&f, "tny mcp list"));
    ASSERT_EQ(0, classify(&f, "tny edit --help"));
    ASSERT_EQ(0, classify(&f, "tny edit"));
    ASSERT_EQ(0, classify(&f, "tny edit a.txt b.txt"));
    ASSERT_EQ(0, classify(&f, "tny memory list extra"));
    ASSERT_EQ(0, classify(&f, "tny skill list"));
    ASSERT_EQ(0, classify(&f, "tny mcp call srv"));
    ASSERT_EQ(0, classify(&f, "tny mcp call a/b/c"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt && rm -rf /"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt; echo done"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt > out"));
    ASSERT_EQ(0, classify(&f, "tny edit \"$FILE\""));
    ASSERT_EQ(0, classify(&f, "tny edit *.c"));
    ASSERT_EQ(0, classify(&f, "TNY_PERMISSION_MODE=yolo tny edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt &"));
    ASSERT_EQ(0, classify(&f, "git status"));
    ASSERT_EQ(0, classify(&f, "tnyx edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "tny"));

    fixture_close(&f);
    PASS();
}

TEST decision_table_accepts_the_two_payload_shapes(void) {
    fixture f;
    fixture_open(&f, TNY_MODE_YOLO);
    const char *fence = "*** SEARCH\nold\n*** REPLACE\nnew\n*** END\n";

    tny_intercept *ic = tny_intercept_parse(
        &f.env, "tny edit notes.txt <<'EOF'\n*** SEARCH\nold\n*** REPLACE\nnew\n*** END\nEOF\n");
    ASSERT(ic && ic->kind == TNY_INTERCEPT_EDIT);
    ASSERT_STR_EQ(fence, ic->stdin_data);
    ASSERT_STR_EQ("tny edit notes.txt", ic->label);
    ASSERT_STR_EQ("edit_file", ic->permission_tool);
    tny_intercept_free(ic);

    /* an unquoted delimiter is fine while the body has nothing to expand */
    ic = tny_intercept_parse(&f.env, "tny edit notes.txt <<EOF\n*** SEARCH\nold\n*** REPLACE\n"
                                     "new\n*** END\nEOF");
    ASSERT(ic && ic->kind == TNY_INTERCEPT_EDIT);
    ASSERT_STR_EQ(fence, ic->stdin_data);
    tny_intercept_free(ic);
    /* …and refused as soon as it does */
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt <<EOF\n*** SEARCH\n$HOME\n*** REPLACE\n"
                              "new\n*** END\nEOF\n"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt <<'EOF'\nno terminator\n"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt <<-EOF\nx\nEOF\n"));
    ASSERT_EQ(0, classify(&f, "tny edit notes.txt <<'EOF'\nx\nEOF\nrm -rf /\n"));

    ic = tny_intercept_parse(
        &f.env, "printf '*** SEARCH\\nold\\n*** REPLACE\\nnew\\n*** END\\n' | tny edit notes.txt");
    ASSERT(ic && ic->kind == TNY_INTERCEPT_EDIT);
    ASSERT_STR_EQ(fence, ic->stdin_data);
    tny_intercept_free(ic);

    ic = tny_intercept_parse(&f.env, "echo '{\"a\":1}' | tny mcp call srv/deploy");
    ASSERT(ic && ic->kind == TNY_INTERCEPT_MCP_CALL);
    ASSERT_STR_EQ("{\"a\":1}\n", ic->stdin_data);
    ASSERT_STR_EQ("mcp:srv/deploy", ic->permission_tool);
    ASSERT_STR_EQ("tny mcp call srv/deploy", ic->label);
    tny_intercept_free(ic);

    write_workspace_file("args.json", "{\"b\":2}");
    ic = tny_intercept_parse(&f.env, "cat args.json | tny mcp call srv/deploy");
    ASSERT(ic && ic->kind == TNY_INTERCEPT_MCP_CALL);
    ASSERT_STR_EQ("{\"b\":2}", ic->stdin_data);
    tny_intercept_free(ic);

    /* producers the intercept cannot reproduce byte-for-byte */
    ASSERT_EQ(0, classify(&f, "printf '%s' hello | tny edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "printf 'a\\0101' | tny edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "echo 'a\\tb' | tny edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "cat /etc/hosts | tny edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "cat missing.json | tny mcp call srv/deploy"));
    ASSERT_EQ(0, classify(&f, "sed s/a/b/ f | tny edit notes.txt"));
    ASSERT_EQ(0, classify(&f, "echo x | tny edit notes.txt | tee log"));
    ASSERT_EQ(0, classify(&f, "echo x || tny edit notes.txt"));
    fixture_close(&f);
    PASS();
}

/* ---- permission parity with the typed tools ---- */

TEST intercepted_verbs_borrow_the_typed_tools_identity(void) {
    ensure_env();
    write_settings("{}");
    fixture f;
    fixture_open(&f, TNY_MODE_ASK);
    tools_call call, typed;

    ASSERT_EQ(
        0, tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny edit notes.txt\"}", &call));
    ASSERT_EQ(0, tools_call_prepare(&f.env, "edit_file",
                                    "{\"path\":\"notes.txt\",\"old_string\":\"a\","
                                    "\"new_string\":\"b\"}",
                                    &typed));
    ASSERT_STR_EQ("edit_file", call.permission_tool);
    ASSERT_STR_EQ(typed.detail, call.detail);
    ASSERT_EQ(typed.verdict, call.verdict);
    ASSERT_STR_EQ("tny edit notes.txt", tools_call_label(&call));
    ASSERT_EQ(NULL, tools_call_label(&typed));
    ASSERT(call.summary && strstr(call.summary, "tny edit notes.txt -> edit_file"));
    tools_call_free(&typed);
    tools_call_free(&call);

    /* a granted intercepted edit is a granted edit_file, not a shell grant */
    ASSERT_EQ(
        0, tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny edit notes.txt\"}", &call));
    tools_call_grant(&f.env, &call);
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&f.env, "edit_file",
                                    "{\"path\":\"notes.txt\",\"old_string\":\"a\","
                                    "\"new_string\":\"b\"}",
                                    &typed));
    ASSERT_EQ(PERM_ALLOW, typed.verdict);
    tools_call_free(&typed);

    ASSERT_EQ(0, tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny mcp call srv/deploy\"}",
                                    &call));
    ASSERT_STR_EQ("mcp:srv/deploy", call.permission_tool);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);

    ASSERT_EQ(0,
              tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny memory list\"}", &call));
    ASSERT_STR_EQ("memory", call.permission_tool);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);

    /* the read-only verbs stay free, exactly like their typed tools */
    ASSERT_EQ(
        0, tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny skill show demo\"}", &call));
    ASSERT_STR_EQ("skill", call.permission_tool);
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);
    ASSERT_EQ(0,
              tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny ask-user hi\"}", &call));
    ASSERT_STR_EQ("ask_user_question", call.permission_tool);
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);

    /* an ordinary command keeps the terminal identity and the full command */
    ASSERT_EQ(0, tools_call_prepare(&f.env, "terminal", "{\"command\":\"git status\"}", &call));
    ASSERT_STR_EQ("terminal", call.permission_tool);
    ASSERT_STR_EQ("git status", call.detail);
    ASSERT_EQ(NULL, tools_call_label(&call));
    tools_call_free(&call);

    /* background keeps its detached contract: never intercepted */
    ASSERT_EQ(0, tools_call_prepare(&f.env, "terminal",
                                    "{\"command\":\"tny edit notes.txt\",\"background\":true}",
                                    &call));
    ASSERT_STR_EQ("terminal", call.permission_tool);
    ASSERT_EQ(NULL, tools_call_label(&call));
    tools_call_free(&call);

    fixture_close(&f);

    /* an edit rule that denies the path denies the verb too */
    write_settings("{\"permission\":{\"edit\":{\"*/notes.txt\":\"deny\"}}}");
    fixture_open(&f, TNY_MODE_ASK);
    ASSERT_EQ(
        0, tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny edit notes.txt\"}", &call));
    ASSERT_EQ(PERM_DENY, call.verdict);
    tools_call_free(&call);
    char *denied = run_terminal(&f, "tny edit notes.txt");
    ASSERT(strstr(denied, "permission denied"));
    free(denied);
    fixture_close(&f);
    write_settings("{}");
    PASS();
}

TEST foreground_nested_ask_is_refused_with_the_background_recipe(void) {
    fixture f;
    fixture_open(&f, TNY_MODE_YOLO);
    tools_call call;
    ASSERT_EQ(-1, tools_call_prepare(&f.env, "terminal", "{\"command\":\"tny ask hello\"}", &call));
    ASSERT(call.error && strstr(call.error, "tny ask -B"));
    ASSERT(strstr(call.error, "--wait"));
    tools_call_free(&call);
    char *result = run_terminal(&f, "tny ask hello");
    ASSERT(str_starts(result, "error: "));
    ASSERT(strstr(result, "tny ask -B"));
    free(result);
    fixture_close(&f);
    PASS();
}

/* ---- execution ---- */

TEST intercepted_edit_matches_the_cli_and_records_undo(void) {
    fixture f;
    fixture_open(&f, TNY_MODE_YOLO);
    write_workspace_file("notes.txt", "alpha\nold line\nomega\n");

    char *result = run_terminal(&f, "tny edit notes.txt <<'EOF'\n*** SEARCH\nold line\n"
                                    "*** REPLACE\nnew line\n*** END\nEOF\n");
    ASSERT_STR_EQ("exit: 0\nedited notes.txt: replaced 1 occurrence\n", result);
    free(result);
    char *body = read_workspace_file("notes.txt");
    ASSERT_STR_EQ("alpha\nnew line\nomega\n", body);
    free(body);

    /* /undo reaches an intercepted edit exactly like the edit_file tool */
    char *undone = tools_undo_last(&f.env);
    ASSERT(strstr(undone, "restored"));
    free(undone);
    body = read_workspace_file("notes.txt");
    ASSERT_STR_EQ("alpha\nold line\nomega\n", body);
    free(body);

    /* zero matches: exit 2 plus the nearest unique context, as the CLI prints */
    result = run_terminal(&f, "tny edit notes.txt <<'EOF'\n*** SEARCH\nold lyne\n"
                              "*** REPLACE\nx\n*** END\nEOF\n");
    ASSERT(str_starts(result, "exit: 2\n"));
    ASSERT(strstr(result, "tny: edit: 0 matches in notes.txt\n"));
    ASSERT(strstr(result, "nearest unique context is line 2: old line"));
    free(result);

    /* --json is the machine contract on both sides */
    result = run_terminal(&f, "printf '{\"old\":\"omega\",\"new\":\"finis\"}' "
                              "| tny edit --json notes.txt");
    ASSERT_STR_EQ("exit: 0\n{\"kind\":\"edit\",\"path\":\"notes.txt\","
                  "\"matches\":1,\"replaced\":1}\n",
                  result);
    free(result);

    /* a malformed payload is the CLI's usage failure, not a hang on stdin */
    result = run_terminal(&f, "tny edit notes.txt");
    ASSERT(str_starts(result, "exit: 1\n"));
    ASSERT(strstr(result, "tny: edit: could not read stdin"));
    ASSERT(strstr(result, "Example: printf"));
    free(result);

    fixture_close(&f);
    PASS();
}

TEST intercepted_memory_and_skill_run_the_builtin_tools(void) {
    fixture f;
    fixture_open(&f, TNY_MODE_YOLO);

    char *result = run_terminal(&f, "tny memory set project 'ships on friday'");
    ASSERT_STR_EQ("exit: 0\nremembered project\n", result);
    free(result);
    result = run_terminal(&f, "tny memory get project");
    ASSERT_STR_EQ("exit: 0\nships on friday\n", result);
    free(result);
    result = run_terminal(&f, "tny memory list");
    ASSERT(strstr(result, "project: ships on friday"));
    free(result);
    result = run_terminal(&f, "tny memory get missing");
    ASSERT(str_starts(result, "exit: 2\n"));
    ASSERT(strstr(result, "no memory named missing"));
    free(result);

    /* the value may also arrive on stdin, like every other verb payload */
    result = run_terminal(&f, "printf 'from stdin' | tny memory set piped");
    ASSERT_STR_EQ("exit: 0\nremembered piped\n", result);
    free(result);

    char *skills = path_join(g_home, ".tny/skills/demo");
    mkdir_p(skills);
    char *skill_file = path_join(skills, "SKILL.md");
    file_write_atomic(skill_file, "---\nname: demo\n---\nDEMO BODY\n", 30);
    free(skill_file);
    free(skills);
    result = run_terminal(&f, "tny skill show demo");
    ASSERT(str_starts(result, "exit: 0\n"));
    ASSERT(strstr(result, "DEMO BODY"));
    free(result);
    result = run_terminal(&f, "tny skill show absent");
    ASSERT(str_starts(result, "exit: 2\n"));
    ASSERT(strstr(result, "no skill named absent"));
    free(result);

    fixture_close(&f);
    PASS();
}

static char *fixed_answer(const char *question, void *ud) {
    (void)question;
    return xstrdup((const char *)ud);
}

TEST intercepted_control_verbs_bypass_the_socket(void) {
    fixture f;
    fixture_open(&f, TNY_MODE_YOLO);

    /* no frontend: the CLI's clean refusal, not a hang on a missing socket */
    char *result = run_terminal(&f, "tny ask-user 'which branch?'");
    ASSERT_STR_EQ("exit: 2\ntny: no interactive owner is attached\n", result);
    free(result);

    f.env.ask_user = fixed_answer;
    f.env.ask_user_ud = (void *)"main";
    result = run_terminal(&f, "tny ask-user 'which branch?'");
    ASSERT_STR_EQ("exit: 0\nmain\n", result);
    free(result);
    result = run_terminal(&f, "tny --json ask-user 'which branch?'");
    ASSERT(str_starts(result, "exit: 0\n{\"kind\":\"ask_user\","));
    ASSERT(strstr(result, "\"answer\":\"main\"}"));
    free(result);

    /* image attach queues through the same path as read_image */
    static const unsigned char png[] = {0x89, 'P',  'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0,
                                        0,    0x0d, 'I', 'H', 'D',  'R',  0,    0,    0, 1,
                                        0,    0,    0,   1,   8,    6,    0,    0,    0};
    char *shot = path_join(g_ws, "shot.png");
    file_write_atomic(shot, png, sizeof png);
    free(shot);
    result = run_terminal(&f, "tny image attach shot.png");
    ASSERT_STR_EQ("exit: 0\n", result);
    free(result);
    ASSERT_EQ(1, f.env.n_pending_images);
    result = run_terminal(&f, "tny --json image attach shot.png");
    ASSERT(str_starts(result, "exit: 0\n{\"kind\":\"image_attach\","));
    ASSERT(strstr(result, "\"ok\":true}"));
    free(result);
    ASSERT_EQ(2, f.env.n_pending_images);

    /* outside the allowed roots the socket op refuses, and so does this one */
    result = run_terminal(&f, "tny image attach /etc/hosts");
    ASSERT(str_starts(result, "exit: 2\n"));
    ASSERT(strstr(result, "outside the allowed roots"));
    free(result);

    fixture_close(&f);
    PASS();
}

/* ---- nested runs cannot widen the parent's mode ---- */

TEST nested_runs_cannot_widen_the_permission_mode(void) {
    char message[256] = {0};
    unsetenv("TNY_NESTED");
    unsetenv("TNY_NESTED_MODE");
    tny_perm_mode parent = TNY_MODE_YOLO;
    ASSERT(!tny_nested_perm_mode(&parent));
    ASSERT(tny_perm_mode_allowed_nested(TNY_MODE_YOLO, message, sizeof message));

    setenv("TNY_NESTED", "1", 1);
    setenv("TNY_NESTED_MODE", "auto", 1);
    ASSERT(tny_nested_perm_mode(&parent));
    ASSERT_EQ(TNY_MODE_AUTO, parent);
    ASSERT(tny_perm_mode_allowed_nested(TNY_MODE_ASK, message, sizeof message));
    ASSERT(tny_perm_mode_allowed_nested(TNY_MODE_AUTO, message, sizeof message));
    ASSERT(!tny_perm_mode_allowed_nested(TNY_MODE_YOLO, message, sizeof message));
    ASSERT(strstr(message, "wider"));
    ASSERT(strstr(message, "'yolo'") && strstr(message, "'auto'"));

    /* an unset or unknown parent mode is the narrowest one */
    setenv("TNY_NESTED_MODE", "bogus", 1);
    ASSERT(tny_nested_perm_mode(&parent));
    ASSERT_EQ(TNY_MODE_ASK, parent);
    setenv("TNY_NESTED", "0", 1);
    ASSERT(!tny_nested_perm_mode(&parent));

    /* settings are clamped silently; an explicit environment ask is refused */
    ensure_env();
    write_settings("{\"permission_mode\":\"yolo\"}");
    setenv("TNY_NESTED", "1", 1);
    setenv("TNY_NESTED_MODE", "ask", 1);
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(TNY_MODE_ASK, ctx->perm_mode);
    tny_ctx_free(ctx);
    setenv("TNY_PERMISSION_MODE", "yolo", 1);
    ASSERT_EQ(NULL, tny_ctx_load(g_ws));
    unsetenv("TNY_PERMISSION_MODE");
    unsetenv("TNY_NESTED");
    unsetenv("TNY_NESTED_MODE");
    write_settings("{}");
    PASS();
}

SUITE(intercept_suite) {
    RUN_TEST(shellwords_splits_quoting_like_sh);
    RUN_TEST(shellwords_stops_at_every_shell_operator);
    RUN_TEST(shellwords_refuses_what_it_cannot_reproduce);
    RUN_TEST(shellwords_expands_only_the_tilde_form_tny_resolves);
    RUN_TEST(decision_table_intercepts_only_documented_shapes);
    RUN_TEST(decision_table_accepts_the_two_payload_shapes);
    RUN_TEST(intercepted_verbs_borrow_the_typed_tools_identity);
    RUN_TEST(foreground_nested_ask_is_refused_with_the_background_recipe);
    RUN_TEST(intercepted_edit_matches_the_cli_and_records_undo);
    RUN_TEST(intercepted_memory_and_skill_run_the_builtin_tools);
    RUN_TEST(intercepted_control_verbs_bypass_the_socket);
    RUN_TEST(nested_runs_cannot_widen_the_permission_mode);
}
