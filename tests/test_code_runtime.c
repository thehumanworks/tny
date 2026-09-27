#include "greatest.h"
#include "core/code_policy.h"
#include "core/code_runtime.h"
#include "util/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Every cell below runs through the production path: a fresh `--code-cell`
 * child of this test binary with the OS sandbox, not an in-process shortcut. */

static char *fake_tool(void *userdata, const char *name, const char *args) {
    unsigned *count = userdata;
    ++*count;
    if (!strcmp(name, "denied")) return xstrdup("error: permission denied");
    if (!strcmp(name, "fail")) return NULL;
    if (!strcmp(name, "huge")) {
        char *big = malloc(TNY_CODE_TOOL_RESULT_BYTES + 2);
        if (!big) return NULL;
        memset(big, 'x', TNY_CODE_TOOL_RESULT_BYTES + 1);
        big[TNY_CODE_TOOL_RESULT_BYTES + 1] = 0;
        return big;
    }
    return xstrdup(args);
}

TEST code_composes_calls_and_json(void) {
    unsigned calls = 0;
    char *out = tny_code_run("def twice(n):\n    return n * 2\n"
                             "total = 0\n"
                             "for i in range(1, 4):\n"
                             "    reply = tools.call('echo', json.dumps({'n': twice(i)}))\n"
                             "    total += json.loads(reply)['n']\n"
                             "print(total)\n",
                             5000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("12\n", out);
    ASSERT_EQ(3u, calls);
    free(out);
    PASS();
}

TEST code_state_is_fresh_and_loading_names_absent(void) {
    /* Check the actual builtins dict keys (dir() of a dict lists its methods). */
    const char *script = "absent = [n for n in ('__import__', 'open', 'eval', 'exec', 'compile',\n"
                         "          'input', 'breakpoint') if n not in __builtins__]\n"
                         "try:\n"
                         "    import os\n"
                         "    imported = True\n"
                         "except ImportError:\n"
                         "    imported = False\n"
                         "try:\n"
                         "    saved\n"
                         "    fresh = False\n"
                         "except NameError:\n"
                         "    fresh = True\n"
                         "saved = 42\n"
                         "# Control: the same membership test sees keys that must be present.\n"
                         "present = all(n in __builtins__ for n in ('len', 'print', 'sorted', "
                         "'type'))\n"
                         "print(len(absent), imported, fresh, present)\n";
    for (int i = 0; i < 2; ++i) {
        char *out = tny_code_run(script, 5000, NULL, NULL, NULL);
        ASSERT(out);
        ASSERT_STR_EQ("7 False True True\n", out);
        free(out);
    }
    PASS();
}

TEST code_limits_are_enforced(void) {
    const char *scripts[] = {
        "while True:\n    pass\n",
        "print('x' * 65537)\n",
        "data = []\nwhile True:\n    data.append('x' * 1000000)\n",
        "for _ in range(65):\n    tools.call('echo', '{}')\n",
        "tools.call('run_code', '{}')\n",
        "tools.call('echo', '[]')\n",
        "tools.call('echo', {'path': 'x'})\n",
        "t = []\nt.append(t)\njson.dumps(t)\n",
        "raise ValueError('boom')\n",
        "print('hello\\x00')\n",
        "local x = 1\n",
        "tools.call('fail', '{}')\n",
        "tools.call('huge', '{}')\n",
    };
    for (size_t i = 0; i < sizeof(scripts) / sizeof(*scripts); ++i) {
        unsigned calls = 0;
        char *out = tny_code_run(scripts[i], 1000, NULL, fake_tool, &calls);
        ASSERT(out);
        if (!str_starts(out, "error: code: ")) FAILm(scripts[i]);
        ASSERT(strlen(out) <= TNY_CODE_RESULT_TEXT_BYTES);
        ASSERT(calls <= TNY_CODE_TOOL_CALLS);
        free(out);
    }
    PASS();
}

/* A caught limit never re-enables work: no further nested call happens and
 * the cell still ends in the limit error. */
TEST code_limits_are_sticky(void) {
    struct {
        const char *script, *reason;
        unsigned max_calls;
    } cases[] = {
        {"try:\n    print('x' * 70000)\nexcept BaseException:\n    pass\n"
         "tools.call('echo', '{}')\n",
         "output limit exceeded", 0},
        {"try:\n    data = []\n    while True:\n        data.append('x' * 1000000)\n"
         "except BaseException:\n    data = None\n"
         "tools.call('echo', '{}')\n",
         "memory limit exceeded", 0},
        {"for _ in range(64):\n    tools.call('echo', '{}')\n"
         "try:\n    tools.call('echo', '{}')\nexcept BaseException:\n    pass\n"
         "print('continued')\n",
         "tool call limit exceeded", 64},
        {"try:\n    tools.call('fail', '{}')\nexcept BaseException:\n    pass\n"
         "tools.call('echo', '{}')\n",
         "tool callback failed", 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        unsigned calls = 0;
        char *out = tny_code_run(cases[i].script, 5000, NULL, fake_tool, &calls);
        ASSERT(out);
        if (!str_starts(out, "error: code: ") || !strstr(out, cases[i].reason)) FAILm(out);
        ASSERT(!strstr(out, "continued"));
        ASSERT(calls <= cases[i].max_calls);
        free(out);
    }
    PASS();
}

/* Finalizers run while the cell is cleared or after it; tools are closed. */
TEST code_finalizers_cannot_reach_tools(void) {
    unsigned calls = 0;
    char *out = tny_code_run("class Late:\n"
                             "    def __del__(self):\n"
                             "        tools.call('echo', '{\"late\": true}')\n"
                             "keep = Late()\n"
                             "print('body')\n",
                             5000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("body\n", out);
    ASSERT_EQ(0u, calls);
    free(out);
    PASS();
}

/* Host effects only through tools: a direct write attempt fails inside the
 * cell and leaves no file behind. */
TEST code_cell_has_no_ambient_file_authority(void) {
    char dir[] = "/var/tmp/tny-code-cell-XXXXXX";
    ASSERT(mkdtemp(dir));
    char target[128], code[512];
    snprintf(target, sizeof target, "%s/escape.txt", dir);
    snprintf(code, sizeof code,
             "blocked = 0\n"
             "try:\n    open('%s', 'w')\nexcept Exception:\n    blocked += 1\n"
             "try:\n    __import__('os')\nexcept Exception:\n    blocked += 1\n"
             "print('blocked', blocked)\n",
             target);
    char *out = tny_code_run(code, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("blocked 2\n", out);
    ASSERT(access(target, F_OK) != 0);
    free(out);
    rmdir(dir);
    PASS();
}

TEST code_preserves_denial_and_catalog(void) {
    unsigned calls = 0;
    const char *catalog = "[{\"type\":\"function\",\"function\":{\"name\":\"denied\"}}]";
    char *out = tny_code_run("assert len(json.loads(tools.list())) == 1\n"
                             "entry = json.loads(tools.describe('denied'))\n"
                             "assert entry['function']['name'] == 'denied'\n"
                             "assert tools.describe('missing') is None\n"
                             "print(tools.call('denied', '{}'))\n",
                             5000, catalog, fake_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("error: permission denied\n", out);
    ASSERT_EQ(1u, calls);
    free(out);
    PASS();
}

/* Expected text generated by CPython 3.14.7 with the stdlib json module. */
TEST code_json_facade_matches_cpython(void) {
    struct {
        const char *code, *expected;
    } cases[] = {
        {"print(json.dumps(json.loads('{\"a\":[1,true,null,\"hi\",{}],\"b\":{},\"c\":[],\"z\":0,"
         "\"f\":false,\"e\":\"\"}')))\n",
         "{\"a\": [1, true, null, \"hi\", {}], \"b\": {}, \"c\": [], \"z\": 0, \"f\": false, "
         "\"e\": \"\"}\n"},
        {"v = json.loads('[9007199254740993, -9223372036854775808, 18446744073709551616, "
         "1e400]')\nprint(v, json.dumps(v[:3]))\n",
         "[9007199254740993, -9223372036854775808, 18446744073709551616, inf] [9007199254740993, "
         "-9223372036854775808, 18446744073709551616]\n"},
        {"print(json.dumps({\"k\": [1, 2], \"o\": {\"p\": None}, \"e\": [], \"d\": {}}, "
         "indent=2))\n",
         "{\n  \"k\": [\n    1,\n    2\n  ],\n  \"o\": {\n    \"p\": null\n  },\n  \"e\": [],\n  "
         "\"d\": {}\n}\n"},
        {"print(json.dumps({\"b\": 1, \"a\": [1.0, 0.1, 1e16, -0.0]}, separators=(\",\", \":\"), "
         "sort_keys=True))\n",
         "{\"a\":[1.0,0.1,1e+16,-0.0],\"b\":1}\n"},
        {"s = \"\\u00e9\\U0001f600\\n\\\"\\\\\\x01\\x7f\"\nprint(json.dumps(s), json.dumps(s, "
         "ensure_ascii=False))\n",
         "\"\\u00e9\\ud83d\\ude00\\n\\\"\\\\\\u0001\\u007f\" \"\xc3"
         "\xa9"
         "\xf0"
         "\x9f"
         "\x98"
         "\x80"
         "\\n\\\"\\\\\\u0001\x7f"
         "\"\n"},
        {"print(json.dumps({1: \"a\", True: \"b\", None: \"c\", 2.5: \"d\"}))\n",
         "{\"1\": \"b\", \"null\": \"c\", \"2.5\": \"d\"}\n"},
        {"print(json.dumps((1, (2, 3))), json.dumps({\"x\": {\"b\": 1, \"a\": 2}}))\n",
         "[1, [2, 3]] {\"x\": {\"b\": 1, \"a\": 2}}\n"},
        {"v = json.loads('{\"n\": null, \"f\": false, \"z\": 0}')\nprint(v[\"n\"] is None, "
         "v[\"f\"] is False, type(v[\"z\"]).__name__, v[\"z\"] is False)\n",
         "True True int False\n"},
        {"try:\n    json.loads(\"[1,]\")\nexcept json.JSONDecodeError as e:\n    print(\"decode\", "
         "isinstance(e, ValueError))\n",
         "decode True\n"},
        {"try:\n    json.dumps({\"s\": {1}})\nexcept TypeError:\n    print(\"type\")\n", "type\n"},
        {"try:\n    json.dumps(float(\"nan\"), allow_nan=False)\nexcept ValueError:\n    "
         "print(\"nan\")\nprint(json.dumps([float(\"inf\")]))\n",
         "nan\n[Infinity]\n"},
        {"print(json.loads(b'{\"a\": \"\\\\u00e9\"}')[\"a\"], json.loads('\"\\\\ud83d\\\\ude00\"') "
         "== \"\\U0001f600\")\n",
         "\xc3"
         "\xa9"
         " True\n"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        char *out = tny_code_run(cases[i].code, 5000, NULL, NULL, NULL);
        ASSERT(out);
        if (strcmp(out, cases[i].expected) != 0) FAILm(out);
        free(out);
    }
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
    if (prompt->extend) *prompt->deadline += 3000;
    return xstrdup("approved");
}

TEST code_observes_trusted_prompt_deadline_updates(void) {
    int64_t deadline = monotonic_ms() + 5000;
    prompt_deadline prompt = {.deadline = &deadline, .extend = true};
    char *out = tny_code_run_with_deadline("print(tools.call('prompt', '{}'))\n"
                                           "n = 0\nfor i in range(2000):\n    n += 1\nprint(n)\n",
                                           &deadline, NULL, fake_prompt_wait, &prompt);
    ASSERT(out);
    ASSERT_STR_EQ("approved\n2000\n", out);
    free(out);
    prompt.extend = false;
    deadline = monotonic_ms() + 5000;
    out = tny_code_run_with_deadline("print(tools.call('prompt', '{}'))\n", &deadline, NULL,
                                     fake_prompt_wait, &prompt);
    ASSERT(out);
    ASSERT(strstr(out, "deadline exceeded"));
    free(out);
    PASS();
}

/* Boundaries of the source-linked gates (also proved in Lean). */
TEST code_policy_boundaries(void) {
    ASSERT(!tny_code_timeout_admit(0));
    ASSERT(tny_code_timeout_admit(1));
    ASSERT(tny_code_timeout_admit(TNY_CODE_MAX_TIMEOUT_MS));
    ASSERT(!tny_code_timeout_admit(TNY_CODE_MAX_TIMEOUT_MS + 1));
    ASSERT(tny_code_source_admit(TNY_CODE_SOURCE_BYTES));
    ASSERT(!tny_code_source_admit(TNY_CODE_SOURCE_BYTES + 1));
    ASSERT(tny_code_call_admit(63, 1, false, TNY_CODE_ARGUMENT_BYTES, true));
    ASSERT(!tny_code_call_admit(64, 1, false, 2, true));
    ASSERT(!tny_code_call_admit(0, 0, false, 2, true));
    ASSERT(!tny_code_call_admit(0, TNY_CODE_NAME_BYTES + 1, false, 2, true));
    ASSERT(!tny_code_call_admit(0, 4, true, 2, true));
    ASSERT(!tny_code_call_admit(0, 4, false, 2, false));
    ASSERT(!tny_code_call_admit(0, 4, false, TNY_CODE_ARGUMENT_BYTES + 1, true));
    ASSERT(tny_code_output_admit(0, TNY_CODE_OUTPUT_BYTES));
    ASSERT(!tny_code_output_admit(1, TNY_CODE_OUTPUT_BYTES));
    ASSERT(!tny_code_output_admit(TNY_CODE_OUTPUT_BYTES + 1, 0));
    ASSERT(tny_code_memory_admit(0, 10, 16, 26));
    ASSERT(!tny_code_memory_admit(0, 11, 16, 26));
    ASSERT(!tny_code_memory_admit(UINT64_MAX - 1, 1, 16, UINT64_MAX));
    ASSERT(tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_CALL, 3, 63));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_CALL, 3, 64));
    ASSERT(tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_DONE, 1, 64));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_FINISHED, TNY_CODE_FRAME_DONE, 1, 0));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_RESULT, 2, 0));
    ASSERT(!tny_code_frame_admit(TNY_CODE_PHASE_RUNNING, TNY_CODE_FRAME_DONE,
                                 TNY_CODE_RESULT_TEXT_BYTES + 2, 0));
    ASSERT_EQ(TNY_CODE_JSON_BOOL,
              tny_code_json_kind(false, true, true, false, false, false, false));
    ASSERT_EQ(TNY_CODE_JSON_NULL,
              tny_code_json_kind(true, false, false, false, false, false, false));
    ASSERT_EQ(TNY_CODE_JSON_INT,
              tny_code_json_kind(false, false, true, false, false, false, false));
    ASSERT_EQ(TNY_CODE_JSON_DEFAULT,
              tny_code_json_kind(false, false, false, false, false, false, false));
    PASS();
}

static char *exact_name_tool(void *userdata, const char *name, const char *args) {
    (void)args;
    unsigned *calls = userdata;
    ++*calls;
    return xstrdup(name);
}

TEST code_tool_names_preserve_exact_bytes(void) {
    unsigned calls = 0;
    const char *source = "names = ['echo', 'echo\\n', '\\nleading', 'two\\nlines', 'with:colon', "
                         "'\\u00e9', 'x' * 256]\n"
                         "for name in names:\n"
                         "    assert tools.call(name, '{}') == name\n"
                         "print('exact')\n";
    char *out = tny_code_run(source, 5000, NULL, exact_name_tool, &calls);
    ASSERT(out);
    ASSERT_STR_EQ("exact\n", out);
    ASSERT_EQ(7u, calls);
    free(out);
    PASS();
}

TEST code_json_reentrant_container_lifetimes(void) {
    const char *source = "values = [object(), object()]\n"
                         "def shrink(value):\n"
                         "    values.clear()\n"
                         "    return 7\n"
                         "assert json.dumps(values, default=shrink) == '[7]'\n"
                         "class Broken(dict):\n"
                         "    def items(self):\n"
                         "        return [1]\n"
                         "try:\n"
                         "    json.dumps(Broken(a=1))\n"
                         "except (TypeError, ValueError):\n"
                         "    pass\n"
                         "else:\n"
                         "    raise AssertionError('malformed item was accepted')\n"
                         "pairs = [('a', object()), ('b', 2)]\n"
                         "class Mutable(dict):\n"
                         "    def items(self):\n"
                         "        return pairs\n"
                         "def change(value):\n"
                         "    pairs.clear()\n"
                         "    return 3\n"
                         "assert json.loads(json.dumps(Mutable(a=1), default=change)) == {'a': 3}\n"
                         "print('containers')\n";
    char *out = tny_code_run(source, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("containers\n", out);
    free(out);
    PASS();
}

TEST code_json_error_metadata_and_describe_boundaries(void) {
    const char *source = "text = '[1,]'\n"
                         "try:\n"
                         "    json.loads(text)\n"
                         "except json.JSONDecodeError as error:\n"
                         "    assert error.doc == text\n"
                         "    assert isinstance(error.msg, str)\n"
                         "    assert error.lineno == 1 and error.colno >= 1 and error.pos >= 0\n"
                         "assert json.dumps({'a': 1}, separators=[',', ':']) == '{\"a\":1}'\n"
                         "try:\n"
                         "    tools.describe('echo\\x00other')\n"
                         "except ValueError:\n"
                         "    pass\n"
                         "else:\n"
                         "    raise AssertionError('NUL name silently truncated')\n"
                         "assert tools.describe('echo') is not None\n"
                         "print('metadata')\n";
    char *out = tny_code_run(source, 5000, "[{\"name\":\"echo\"}]", NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("metadata\n", out);
    free(out);
    PASS();
}

TEST code_fatal_quota_does_not_run_finally_or_following_bytecode(void) {
    unsigned calls = 0;
    const char *source = "try:\n"
                         "    print('x' * 70000)\n"
                         "finally:\n"
                         "    tools.call('echo', '{\"finally\":true}')\n"
                         "tools.call('echo', '{\"after\":true}')\n";
    char *out = tny_code_run(source, 5000, NULL, fake_tool, &calls);
    ASSERT(out);
    ASSERT(strstr(out, "output limit exceeded"));
    ASSERT_EQ(0u, calls);
    free(out);
    out = tny_code_run("print('fresh')\n", 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("fresh\n", out);
    free(out);
    PASS();
}

TEST code_unicode_parser_is_self_contained(void) {
    const char *source = "\u03c0 = 3\n"
                         "print(\u03c0, '\\N{GREEK SMALL LETTER ALPHA}', '\\N{GRINNING FACE}')\n"
                         "\u212a = 7\n"
                         "assert K == 7\n"
                         "try:\n"
                         "    import unicodedata\n"
                         "except ImportError:\n"
                         "    print('no import authority')\n"
                         "else:\n"
                         "    raise AssertionError('parser module exposed an import capability')\n";
    char *out = tny_code_run(source, 5000, NULL, NULL, NULL);
    ASSERT(out);
    ASSERT_STR_EQ("3 \u03b1 \U0001f600\nno import authority\n", out);
    free(out);
    PASS();
}

SUITE(code_runtime_suite) {
    RUN_TEST(code_unicode_parser_is_self_contained);
    RUN_TEST(code_tool_names_preserve_exact_bytes);
    RUN_TEST(code_json_reentrant_container_lifetimes);
    RUN_TEST(code_json_error_metadata_and_describe_boundaries);
    RUN_TEST(code_fatal_quota_does_not_run_finally_or_following_bytecode);
    RUN_TEST(code_composes_calls_and_json);
    RUN_TEST(code_state_is_fresh_and_loading_names_absent);
    RUN_TEST(code_limits_are_enforced);
    RUN_TEST(code_limits_are_sticky);
    RUN_TEST(code_finalizers_cannot_reach_tools);
    RUN_TEST(code_cell_has_no_ambient_file_authority);
    RUN_TEST(code_preserves_denial_and_catalog);
    RUN_TEST(code_json_facade_matches_cpython);
    RUN_TEST(code_observes_trusted_prompt_deadline_updates);
    RUN_TEST(code_policy_boundaries);
}
