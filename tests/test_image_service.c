#include "greatest.h"
#include "core/image_provider.h"
#include "core/intercept.h"
#include "core/tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static tny_ctx ctx;
static char root[256];
static bool cancelled(void *ud) {
    (void)ud;
    return true;
}

TEST image_capability_and_validation(void) {
    char err[256];
    ASSERT(!tny_image_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_token = "fixture-image-token";
    ASSERT(!tny_image_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_account_id = "fixture-account";
    ASSERT(tny_image_available(&ctx, NULL, false, err, sizeof err));
    ASSERT(tny_image_available(&ctx, "codex", true, err, sizeof err));
    buf_t names;
    buf_init(&names);
    ASSERT(tny_image_capabilities(&ctx, true, &names));
    ASSERT_STR_EQ("codex", names.data);
    buf_free(&names);
    ASSERT(!tny_image_available(&ctx, "grok", false, err, sizeof err));
    ctx.chatgpt_token = "bad\r\nheader";
    ASSERT(!tny_image_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_token = "fixture-image-token";
    tny_image_request r = {.prompt = "hello", .output_file = "unused", .cancelled = cancelled};
    tny_image_result result;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    const char *bad[] = {NULL, "", " \t\n", "\xff"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r.prompt = bad[i];
        ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    }
    char *prompt = malloc(TNY_IMAGE_PROMPT_MAX + 2);
    ASSERT(prompt);
    memset(prompt, 'x', TNY_IMAGE_PROMPT_MAX + 1);
    prompt[TNY_IMAGE_PROMPT_MAX + 1] = 0;
    r.prompt = prompt;
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    prompt[TNY_IMAGE_PROMPT_MAX] = 0;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    free(prompt);
    r.prompt = "hello";
    r.edit = true;
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.images[0] = "input.png";
    r.image_count = 1;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.image_count = 5;
    ASSERT_EQ(130, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.image_count = 6;
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    r.edit = false;
    r.image_count = 0;
    r.quality = "bogus";
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    PASS();
}

TEST image_base64_is_strict_and_magic_checked(void) {
    char err[256];
    const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13};
    buf_t b;
    buf_init(&b);
    b64_encode(png, sizeof png, &b);
    buf_t out;
    buf_init(&out);
    ASSERT_EQ(0, tny_image_decode(b.data, b.len, &out, err, sizeof err));
    ASSERT_EQ(sizeof png, out.len);
    ASSERT_MEM_EQ(png, out.data, sizeof png);
    buf_free(&out);
    buf_free(&b);
    const char *bad[] = {NULL,
                         "",
                         "!!!!",
                         "AAA",
                         "AAAA=AAA",
                         "iVBORw0KGgoAAAAN====",
                         "iVBORw0KGgoAAAAN\n",
                         "R0lGODlhAAAAAAAB",
                         "YWJjZGVmZ2hpamts",
                         "AB==",
                         "AAB="};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        buf_init(&out);
        ASSERT_EQ(1, tny_image_decode(bad[i], bad[i] ? strlen(bad[i]) : 0, &out, err, sizeof err));
        ASSERT_EQ(0, out.len);
        buf_free(&out);
    }
    PASS();
}

TEST image_tool_schema_permission_and_interception(void) {
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    const char *args = "{\"prompt\":\"hello\",\"output_file\":\"out.png\",\"images\":[\"first."
                       "png\",\"second.png\"]}";
    char *schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(strstr(schema, "image_generate"));
    ASSERT(strstr(schema, "image_edit"));
    free(schema);
    tools_call call;
    ASSERT_EQ(0, tools_call_prepare(&env, "image_edit", args, &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    ASSERT(strstr(call.summary, "out.png"));
    ASSERT(strstr(call.summary, "first.png"));
    ASSERT(strstr(call.summary, "second.png"));
    ASSERT(strstr(call.summary, "codex"));
    tools_call_grant(&env, &call);
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "image_edit", args, &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);
    ASSERT_EQ(0,
              tools_call_prepare(
                  &env, "image_edit",
                  "{\"prompt\":\"hello\",\"output_file\":\"out.png\",\"images\":[\"other.png\"]}",
                  &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);
    const char *bad[] = {
        "{\"prompt\":3,\"output_file\":\"out.png\"}",
        "{\"prompt\":\"x\\u0000y\",\"output_file\":\"out.png\"}",
        "{\"prompt\":\"x\",\"output_file\":\"out.png\",\"images\":[3]}",
        "{\"prompt\":\"x\",\"output_file\":\"out.png\",\"images\":[\"x\\u0000y\"]}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", bad[i], &call));
        tools_call_free(&call);
    }
    ctx.tool_profile = TNY_TOOLS_TERMINAL;
    schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(!strstr(schema, "image_generate"));
    free(schema);
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", args, &call));
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "terminal",
                                    "{\"command\":\"printf 'hello' | tny image edit --image "
                                    "first.png --image second.png --output-file out.png --json\"}",
                                    &call));
    ASSERT(call.intercept);
    ASSERT_EQ(TNY_INTERCEPT_IMAGE_RENDER, call.intercept->kind);
    ASSERT_STR_EQ("image_edit", call.permission_tool);
    ASSERT_EQ(PERM_ALLOW, call.verdict); /* same grant as the typed call */
    tools_call_free(&call);
    ctx.tool_profile = TNY_TOOLS_ALL;
    ctx.library_mode = true;
    schema = tools_schema_json(&env);
    ASSERT(schema);
    ASSERT(!strstr(schema, "image_edit"));
    free(schema);
    ctx.library_mode = false;
    ctx.ssh_host = "fixture";
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", args, &call));
    tools_call_free(&call);
    ctx.ssh_host = NULL;
    ctx.chatgpt_token = NULL;
    ASSERT_EQ(-1, tools_call_prepare(&env, "image_edit", args, &call));
    tools_call_free(&call);
    perm_free(env.perm);
    PASS();
}

TEST image_decode_exact_output_limit(void) {
    size_t max = TNY_IMAGE_OUTPUT_MAX;
    unsigned char *data = calloc(1, max + 1);
    ASSERT(data);
    memcpy(data, "\x89PNG\r\n\x1a\n", 8);
    char err[256];
    buf_t encoded, out;
    buf_init(&encoded);
    buf_init(&out);
    b64_encode(data, max, &encoded);
    ASSERT_EQ(0, tny_image_decode(encoded.data, encoded.len, &out, err, sizeof err));
    ASSERT_EQ(max, out.len);
    buf_free(&encoded);
    buf_free(&out);
    buf_init(&encoded);
    buf_init(&out);
    b64_encode(data, max + 1, &encoded);
    ASSERT_EQ(1, tny_image_decode(encoded.data, encoded.len, &out, err, sizeof err));
    ASSERT_EQ(0, out.len);
    buf_free(&encoded);
    buf_free(&out);
    free(data);
    PASS();
}

TEST image_invalid_later_reference_is_an_error(void) {
    ctx.chatgpt_token = "fixture-image-token";
    ctx.chatgpt_account_id = "fixture-account";
    char *input = path_join(root, "input.png");
    ASSERT(input);
    const char bytes[] = "\x89PNG\r\n\x1a\nABCD";
    ASSERT_EQ(0, file_write_atomic(input, bytes, sizeof bytes - 1));
    tny_image_request r = {.edit = true,
                           .prompt = "edit",
                           .output_file = "unused",
                           .images = {input, "\xff"},
                           .image_count = 2};
    tny_image_result result;
    char err[256];
    ASSERT_EQ(1, tny_image_run(&ctx, &r, &result, err, sizeof err));
    ASSERT(strstr(err, "reference path"));
    ASSERT_EQ(0, result.bytes);
    unlink(input);
    free(input);
    PASS();
}

SUITE(image_service_suite) {
    const char *vars[] = {"HOME", "CODEX_HOME", "CHATGPT_ACCESS_TOKEN", "CHATGPT_ACCOUNT_ID"};
    char *saved[4];
    for (size_t i = 0; i < 4; i++) {
        const char *v = getenv(vars[i]);
        saved[i] = v ? xstrdup(v) : NULL;
        unsetenv(vars[i]);
    }
    snprintf(root, sizeof root, "/tmp/tny-image-unit-XXXXXX");
    if (!mkdtemp(root)) abort();
    setenv("HOME", root, 1);
    setenv("CODEX_HOME", root, 1);
    ctx = (tny_ctx){.cwd = root, .perm_mode = TNY_MODE_ASK, .tool_profile = TNY_TOOLS_ALL};
    RUN_TEST(image_capability_and_validation);
    RUN_TEST(image_base64_is_strict_and_magic_checked);
    RUN_TEST(image_decode_exact_output_limit);
    RUN_TEST(image_invalid_later_reference_is_an_error);
    RUN_TEST(image_tool_schema_permission_and_interception);
    rmdir(root);
    for (size_t i = 0; i < 4; i++) {
        if (saved[i]) setenv(vars[i], saved[i], 1);
        else unsetenv(vars[i]);
        free(saved[i]);
    }
}
