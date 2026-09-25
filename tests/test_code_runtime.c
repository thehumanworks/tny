#include "greatest.h"
#include "core/code_runtime.h"
#include "util/util.h"
#include <stdlib.h>
#include <string.h>

static char *fake_tool(void *userdata, const char *name, const char *args) {
    unsigned *count = userdata;
    ++*count;
    if (!strcmp(name, "denied")) return xstrdup("error: permission denied");
    return xstrdup(args);
}
TEST code_composes_calls_and_json(void) {
    unsigned calls = 0;
    char *out = tny_code_run("local sum = 0; local function twice(n) return n * 2 end; "
                             "for i = 1, 3 do local v = json.decode(tools.call('echo', "
                             "json.encode({n=twice(i)}))); sum = sum + v.n end; print(sum)",
                             1000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("12\n", out);
    ASSERT_EQ(3u, calls);
    free(out);
    PASS();
}
TEST code_state_is_fresh_and_capabilities_absent(void) {
    const char *script =
        "assert(io == nil and os == nil and package == nil and debug == nil); "
        "assert(load == nil and loadfile == nil and dofile == nil and require == nil); "
        "assert(coroutine == nil and pcall == nil and xpcall == nil); "
        "assert(setmetatable == nil and string.dump == nil); "
        "assert(saved == nil); saved = 42; return 'ok'";
    for (int i = 0; i < 2; ++i) {
        char *out = tny_code_run(script, 1000, NULL, NULL, NULL);
        ASSERT(out);
        ASSERT_STR_EQ("ok\n", out);
        free(out);
    }
    PASS();
}
TEST code_limits_are_enforced(void) {
    const char *scripts[] = {
        "while true do end",
        "print(string.rep('x', 65537))",
        "local s = string.rep('x', 16777216)",
        "for i=1,65 do tools.call('echo', '{}') end",
        "tools.call('run_code', '{}')",
        "tools.call('echo', '[]')",
        "local t = {}; t.t = t; json.encode(t)",
        "local t = {}; for i=1,1000000 do t[i] = tostring(i) end; json.encode(t)",
        "error({})",
        "error(42)",
        "print('hello' .. string.char(0))",
        "not valid lua",
        "\033Lua"};
    for (size_t i = 0; i < sizeof(scripts) / sizeof(*scripts); ++i) {
        unsigned calls = 0;
        char *out = tny_code_run(scripts[i], 100, NULL, fake_tool, &calls);
        ASSERT(out);
        ASSERT(str_starts(out, "error: code: "));
        ASSERT(strlen(out) <= TNY_CODE_OUTPUT_BYTES);
        ASSERT(calls <= TNY_CODE_TOOL_CALLS);
        free(out);
    }
    PASS();
}
TEST code_preserves_denial_and_catalog(void) {
    unsigned calls = 0;
    const char *catalog = "[{\"type\":\"function\",\"function\":{\"name\":\"denied\"}}]";
    char *out =
        tny_code_run("assert(#json.decode(tools.list()) == 1); "
                     "assert(json.decode(tools.describe('denied'))['function'].name == 'denied'); "
                     "assert(tools.describe('missing') == nil); print(tools.call('denied', '{}'))",
                     1000, catalog, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("error: permission denied\n", out);
    ASSERT_EQ(1u, calls);
    free(out);
    PASS();
}
TEST code_json_roundtrip(void) {
    char *out =
        tny_code_run("assert(json.encode(json.decode('[]')) == '[]'); "
                     "assert(json.encode({}) == '{}'); "
                     "local t = json.decode('{\"a\":[1,true,null,\"hi\"],\"n\":-9}'); "
                     "assert(t.a[3] == json.null); local r = json.decode(json.encode(t)); "
                     "assert(r.a[1] == 1 and r.a[2] and r.a[4] == 'hi' and r.n == -9); print('ok')",
                     1000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("ok\n", out);
    free(out);
    PASS();
}
typedef struct {
    int64_t *deadline;
    bool extend;
} prompt_deadline;

static char *fake_prompt_wait(void *userdata, const char *name, const char *args) {
    (void)name;
    (void)args;
    prompt_deadline *prompt = userdata;
    /* Deterministically model elapsed prompt time, without a timing-sensitive
     * sleep. Restoring the budget is exclusively a trusted callback action. */
    *prompt->deadline = monotonic_ms() - 1;
    if (prompt->extend) *prompt->deadline += 1000;
    return xstrdup("approved");
}
TEST code_observes_trusted_prompt_deadline_updates(void) {
    int64_t deadline = monotonic_ms() + 1000;
    prompt_deadline prompt = {.deadline = &deadline, .extend = true};
    char *out = tny_code_run_with_deadline("print(tools.call('prompt', '{}')); "
                                           "local n=0; for i=1,2000 do n=n+1 end; print(n)",
                                           &deadline, NULL, fake_prompt_wait, &prompt);
    ASSERT(out);
    ASSERT_STR_EQ("approved\n2000\n", out);
    free(out);
    prompt.extend = false;
    deadline = monotonic_ms() + 1000;
    out = tny_code_run_with_deadline("print(tools.call('prompt', '{}'))", &deadline, NULL,
                                     fake_prompt_wait, &prompt);
    ASSERT(out);
    ASSERT(strstr(out, "deadline exceeded"));
    free(out);
    PASS();
}
SUITE(code_runtime_suite) {
    RUN_TEST(code_composes_calls_and_json);
    RUN_TEST(code_state_is_fresh_and_capabilities_absent);
    RUN_TEST(code_limits_are_enforced);
    RUN_TEST(code_preserves_denial_and_catalog);
    RUN_TEST(code_json_roundtrip);
    RUN_TEST(code_observes_trusted_prompt_deadline_updates);
}
