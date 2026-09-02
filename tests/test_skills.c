/* test_skills.c — skill mention matcher and prompt injection (docs/adr/0056). */
#include "greatest.h"
#include "core/skills.h"
#include "core/session.h"
#include "util/util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const skill_meta NAMES[] = {
    {"foo", "", "/x/foo"},
    {"foobar", "", "/x/foobar"},
    {"foo-bar", "", "/x/foo-bar"},
    {"usr", "", "/x/usr"},
};
#define N_NAMES ((int)(sizeof NAMES / sizeof *NAMES))

static bool mentions(const char *text, const char *name) {
    return skills_mention_pos(text, name) != SIZE_MAX;
}

TEST mention_whole_token_only(void) {
    ASSERT(mentions("/foo", "foo"));
    ASSERT(mentions("$foo", "foo"));
    ASSERT(mentions("please /foo now", "foo"));
    ASSERT(mentions("please\n$foo", "foo"));
    ASSERT(mentions("do $foo.", "foo"));
    ASSERT(mentions("do /foo, then stop", "foo"));
    ASSERT(mentions("(/foo)", "foo") == false); /* not after whitespace */
    ASSERT_FALSE(mentions("/foobar", "foo"));
    ASSERT_FALSE(mentions("/foo-bar", "foo"));
    ASSERT_FALSE(mentions("/foo_bar", "foo"));
    ASSERT_FALSE(mentions("a/foo", "foo"));
    ASSERT_FALSE(mentions("x$foo", "foo"));
    ASSERT_FALSE(mentions("foo", "foo"));
    ASSERT_FALSE(mentions("/usr/bin", "usr"));
    ASSERT_FALSE(mentions("see /usr/local", "usr"));
    ASSERT(mentions("/foo-bar", "foo-bar"));
    ASSERT(mentions("$foo-bar!", "foo-bar"));
    ASSERT_FALSE(mentions("", "foo"));
    ASSERT_FALSE(mentions("/foo", ""));
    ASSERT_EQ(0u, skills_mention_pos("/foo bar", "foo"));
    ASSERT_EQ(4u, skills_mention_pos("run $foo", "foo"));
    PASS();
}

TEST mentioned_orders_by_first_appearance_and_dedupes(void) {
    int out[8];
    int n = skills_mentioned("$foobar then /foo then $foobar and /foo-bar", NAMES, N_NAMES, out, 8);
    ASSERT_EQ(3, n);
    ASSERT_STR_EQ("foobar", NAMES[out[0]].name);
    ASSERT_STR_EQ("foo", NAMES[out[1]].name);
    ASSERT_STR_EQ("foo-bar", NAMES[out[2]].name);
    n = skills_mentioned("nothing here, not even /usr/bin", NAMES, N_NAMES, out, 8);
    ASSERT_EQ(0, n);
    n = skills_mentioned("/foo $foobar", NAMES, N_NAMES, out, 1);
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("foo", NAMES[out[0]].name);
    PASS();
}

/* ---- injection against the fixture workspace ---- */

typedef struct {
    tny_ctx *ctx;
    tny_session_state *session;
    char *old_home;
} fx;

static fx fx_new(void) {
    char root[] = "/tmp/tny-skills-test-XXXXXX";
    if (!mkdtemp(root)) abort();
    fx x = {0};
    const char *home = getenv("HOME");
    x.old_home = home ? xstrdup(home) : NULL;
    setenv("HOME", root, 1);
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) abort();
    char *ws = path_join(cwd, "tests/fixtures/skills-ws");
    char *state = path_join(root, ".tny");
    x.ctx = tny_ctx_new_explicit(ws, state);
    x.ctx->no_save = true;
    x.ctx->library_mode = false; /* the explicit constructor defaults to embed mode */
    free(ws);
    free(state);
    x.session = session_new(x.ctx);
    return x;
}

static void fx_free(fx *x) {
    session_close(x->session);
    tny_ctx_free(x->ctx);
    if (x->old_home) setenv("HOME", x->old_home, 1);
    else unsetenv("HOME");
    free(x->old_home);
}

static void strip_cr(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++)
        if (*r != '\r') *w++ = *r;
    *w = 0;
}

TEST inject_wraps_body_ahead_of_prompt(void) {
    fx x = fx_new();
    char **names = NULL;
    int n = 0;
    char *out = skills_inject(x.ctx, x.session, "please $deploy the branch", true, &names, &n);
    ASSERT(out);
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("deploy", names[0]);
    ASSERT(strncmp(out, "<skill name=\"deploy\" path=\"", 27) == 0);
    /* a Windows checkout without .gitattributes gives the fixture CRLF
     * endings; the body is copied verbatim, so compare without the CRs */
    strip_cr(out);
    ASSERT(strstr(out, "skills-ws/skills/deploy/SKILL.md\">\n---\nname: deploy\n"));
    ASSERT(strstr(
        out, "3. Push the tag; CI deploys to staging.\n</skill>\n\nplease $deploy the branch"));
    ASSERT(!strstr(out, "deploy-prod"));
    ASSERT_STR_EQ("please $deploy the branch",
                  out + strlen(out) - strlen("please $deploy the branch"));
    free(out);
    skills_names_free(names, n);
    fx_free(&x);
    PASS();
}

TEST inject_nothing_without_a_whole_token_mention(void) {
    fx x = fx_new();
    char **names = NULL;
    int n = 0;
    ASSERT_EQ(NULL,
              skills_inject(x.ctx, x.session, "look in /usr/bin and deploy", true, &names, &n));
    ASSERT_EQ(0, n);
    ASSERT_EQ(NULL, skills_inject(x.ctx, x.session, "/deployment", true, &names, &n));
    ASSERT_EQ(NULL, skills_inject(x.ctx, x.session, "$nosuchskill", true, &names, &n));
    ASSERT_EQ(NULL, skills_inject(x.ctx, x.session, "", true, &names, &n));
    fx_free(&x);
    PASS();
}

TEST inject_multiple_in_order_once_each(void) {
    fx x = fx_new();
    char **names = NULL;
    int n = 0;
    char *out = skills_inject(x.ctx, x.session, "/deploy-prod after $deploy; /deploy again", false,
                              &names, &n);
    ASSERT(out);
    ASSERT_EQ(2, n);
    ASSERT_STR_EQ("deploy-prod", names[0]);
    ASSERT_STR_EQ("deploy", names[1]);
    const char *a = strstr(out, "<skill name=\"deploy-prod\"");
    const char *b = strstr(out, "<skill name=\"deploy\"");
    ASSERT(a && b && a < b);
    ASSERT_EQ(b, strstr(b + 1, "<skill name=\"deploy\"") ? NULL : b); /* only once */
    free(out);
    skills_names_free(names, n);
    fx_free(&x);
    PASS();
}

TEST inject_bounds_large_bodies(void) {
    fx x = fx_new();
    x.ctx->max_tool_result_bytes = 64;
    char **names = NULL;
    int n = 0;
    char *host = skills_inject(x.ctx, x.session, "$deploy", false, &names, &n);
    ASSERT(host);
    ASSERT(strstr(host, "…[truncated: 32 of "));
    ASSERT(strstr(host, "read the rest from "));
    ASSERT(!strstr(host, "CI deploys to staging"));
    free(host);
    skills_names_free(names, n);
    names = NULL;
    n = 0;
    char *native = skills_inject(x.ctx, x.session, "$deploy", true, &names, &n);
    ASSERT(native);
    ASSERT(strstr(native, "…[truncated: 32 of "));
    ASSERT(strstr(native, "read_tool_result(handle, offset, length)"));
    ASSERT(strstr(native, "The `skill` tool or read_file on "));
    free(native);
    skills_names_free(names, n);
    fx_free(&x);
    PASS();
}

TEST session_record_dedupes_and_keeps_typed_text(void) {
    fx x = fx_new();
    char **names = NULL;
    int n = 0;
    char *first = skills_inject(x.ctx, x.session, "$deploy now", true, &names, &n);
    ASSERT(first);
    int idx = session_message_count(x.session);
    session_add_text(x.session, "user", first);
    session_record_skill_injection(x.session, idx, names, n, "$deploy now");
    free(first);
    skills_names_free(names, n);
    ASSERT(session_skill_injected(x.session, "deploy"));
    ASSERT_FALSE(session_skill_injected(x.session, "deploy-prod"));
    ASSERT_STR_EQ("$deploy now", session_message_display(x.session, idx));
    ASSERT_EQ(NULL, session_message_display(x.session, idx + 1));

    /* the second mention becomes a reminder, never a second body */
    names = NULL;
    n = 0;
    char *again = skills_inject(x.ctx, x.session, "and $deploy once more", true, &names, &n);
    ASSERT(again);
    ASSERT_EQ(1, n);
    ASSERT(strstr(again, "<skill name=\"deploy\" path=\""));
    ASSERT(strstr(again, "already loaded earlier in this conversation"));
    ASSERT(!strstr(again, "Push the tag"));
    free(again);
    skills_names_free(names, n);

    /* the record rides session.json at the top level, never messages[] */
    char *json = jwrite(x.session->doc);
    ASSERT(json);
    ASSERT(strstr(json, "\"skill_injections\":[{\"message\":0,\"skills\":[\"deploy\"],\"display\":"
                        "\"$deploy now\"}"));
    free(json);
    fx_free(&x);
    PASS();
}

TEST library_mode_never_injects(void) {
    fx x = fx_new();
    x.ctx->library_mode = true;
    char **names = NULL;
    int n = 0;
    ASSERT_EQ(NULL, skills_inject(x.ctx, x.session, "$deploy", true, &names, &n));
    fx_free(&x);
    PASS();
}

SUITE(skills_suite) {
    RUN_TEST(mention_whole_token_only);
    RUN_TEST(mentioned_orders_by_first_appearance_and_dedupes);
    RUN_TEST(inject_wraps_body_ahead_of_prompt);
    RUN_TEST(inject_nothing_without_a_whole_token_mention);
    RUN_TEST(inject_multiple_in_order_once_each);
    RUN_TEST(inject_bounds_large_bodies);
    RUN_TEST(session_record_dedupes_and_keeps_typed_text);
    RUN_TEST(library_mode_never_injects);
}
