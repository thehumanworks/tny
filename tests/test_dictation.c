#include "greatest.h"
#include "core/dictation.h"
#include "tui/tui.h"
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)v;
        v >>= 8;
    }
}
static unsigned char *wav(size_t *len) {
    *len = 44 + 48000;
    unsigned char *p = calloc(1, *len);
    if (!p) abort();
    memcpy(p, "RIFF", 4);
    put32(p + 4, (uint32_t)(*len - 8));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16);
    p[20] = 1;
    p[22] = 1;
    put32(p + 24, 24000);
    put32(p + 28, 48000);
    p[32] = 2;
    p[34] = 16;
    memcpy(p + 36, "data", 4);
    put32(p + 40, 48000);
    return p;
}

TEST dictation_wav_is_complete_pcm_with_duration_bounds(void) {
    size_t n;
    unsigned char *p = wav(&n);
    ASSERT(tny_dictation_wav_valid(p, n));
    ASSERT(!tny_dictation_wav_valid(NULL, n));
    ASSERT(!tny_dictation_wav_valid(p, TNY_DICTATION_AUDIO_MAX + 1));
    ASSERT(!tny_dictation_wav_valid(p, n - 1));
    p[20] = 3;
    ASSERT(!tny_dictation_wav_valid(p, n));
    p[20] = 1;
    p[22] = 0;
    ASSERT(!tny_dictation_wav_valid(p, n));
    p[22] = 1;
    p[34] = 8;
    ASSERT(!tny_dictation_wav_valid(p, n));
    p[34] = 16;
    put32(p + 28, 1);
    ASSERT(!tny_dictation_wav_valid(p, n));
    put32(p + 28, 48000);
    put32(p + 40, 47998);
    put32(p + 4, (uint32_t)n - 10);
    ASSERT(!tny_dictation_wav_valid(p, n - 2)); /* under one second */
    free(p);
    PASS();
}

TEST dictation_wav_rejects_truncated_chunks_and_overflow(void) {
    size_t n;
    unsigned char *p = wav(&n);
    put32(p + 16, UINT32_MAX);
    ASSERT(!tny_dictation_wav_valid(p, n));
    put32(p + 16, 16);
    put32(p + 40, UINT32_MAX);
    ASSERT(!tny_dictation_wav_valid(p, n));
    put32(p + 40, 48000);
    memcpy(p + 12, "JUNK", 4);
    ASSERT(!tny_dictation_wav_valid(p, n)); /* data before format */
    free(p);
    PASS();
}

TEST dictation_text_is_bounded_utf8_without_terminal_controls(void) {
    ASSERT(tny_dictation_text_valid("Hello, 世界.\n", strlen("Hello, 世界.\n")));
    const char *bad[] = {"", " \r\n\t", "x\x1b[2J", "a\x7f", "\xff", "x\xc2\x9b"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        ASSERT(!tny_dictation_text_valid(bad[i], strlen(bad[i])));
    ASSERT(!tny_dictation_text_valid("a\0b", 3));
    char *large = malloc(TNY_DICTATION_TEXT_MAX + 1);
    ASSERT(large);
    memset(large, 'x', TNY_DICTATION_TEXT_MAX + 1);
    ASSERT(tny_dictation_text_valid(large, TNY_DICTATION_TEXT_MAX));
    ASSERT(!tny_dictation_text_valid(large, TNY_DICTATION_TEXT_MAX + 1));
    free(large);
    PASS();
}

TEST dictation_provider_does_not_select_or_rewrite_chat(void) {
    tny_ctx ctx = {.provider_name = "grok",
                   .model = "grok-fixture",
                   .base_url = "http://chat.invalid",
                   .api_key = "chat-only",
                   .chatgpt_token = "dictation-only",
                   .chatgpt_account_id = "fixture-account"};
    char err[256];
    ASSERT(tny_dictation_available(&ctx, "codex", false, err, sizeof err));
    ASSERT_STR_EQ("", err);
    ASSERT(!tny_dictation_available(&ctx, "unknown", false, err, sizeof err));
    ctx.chatgpt_token = "bad\r\nheader";
    ASSERT(!tny_dictation_available(&ctx, "codex", false, err, sizeof err));
    ASSERT_STR_EQ("grok", ctx.provider_name);
    ASSERT_STR_EQ("grok-fixture", ctx.model);
    ASSERT_STR_EQ("http://chat.invalid", ctx.base_url);
    ASSERT_STR_EQ("chat-only", ctx.api_key);
    PASS();
}

TEST dictation_xai_credentials_preserve_chat_and_reject_invalid_flags(void) {
    tny_ctx ctx = {.provider_name = "claude",
                   .model = "chat-model",
                   .base_url = "http://chat.invalid",
                   .api_key = "chat-key",
                   .xai_api_key = "fixture-xai-key"};
    char err[256];
    ASSERT(tny_dictation_available(&ctx, "xai", false, err, sizeof err));
    const char *bad[] = {"", " ", "bad\rkey", "bad\nkey"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ctx.xai_api_key = (char *)bad[i];
        ASSERT(!tny_dictation_available(&ctx, "xai", false, err, sizeof err));
        ASSERT(strstr(err, "--xai-api-key"));
        ASSERT(strstr(err, "XAI_API_KEY"));
        ASSERT(strstr(err, "xai settings profile"));
        ASSERT(strstr(err, "tny --provider grok login"));
    }
    ASSERT_STR_EQ("claude", ctx.provider_name);
    ASSERT_STR_EQ("chat-model", ctx.model);
    ASSERT_STR_EQ("http://chat.invalid", ctx.base_url);
    ASSERT_STR_EQ("chat-key", ctx.api_key);
    PASS();
}

TEST dictation_inserts_at_caret_without_submitting(void) {
    tui t = {0};
    buf_appends(&t.input, "Please review");
    t.cur = 6;
    ASSERT(tui_dictation_insert(&t, "carefully"));
    ASSERT_STR_EQ("Please carefully review", t.input.data);
    ASSERT_EQ(16, t.cur);
    ASSERT(!t.turn_active);
    ASSERT(!t.engine);
    ASSERT(!t.session);
    ASSERT_EQ(0, t.n_queue);
    ASSERT(!tui_dictation_insert(&t, "bad\x1b[2J"));
    ASSERT_STR_EQ("Please carefully review", t.input.data);
    buf_free(&t.input);
    PASS();
}

TEST dictation_composer_capacity_preserves_the_draft(void) {
    tui t = {0};
    buf_reserve(&t.input, 1u << 20);
    ASSERT(!t.input.oom);
    memset(t.input.data, 'x', 1u << 20);
    t.input.len = 1u << 20;
    t.input.data[t.input.len] = 0;
    t.cur = t.input.len;
    ASSERT(!tui_dictation_insert(&t, "extra"));
    ASSERT_EQ(1u << 20, t.input.len);
    ASSERT_EQ(t.input.len, t.cur);
    ASSERT_EQ('x', t.input.data[t.input.len - 1]);
    buf_free(&t.input);
    PASS();
}

TEST dictation_shortcut_decodes_at_every_split(void) {
    const char *seqs[] = {"\x12", "\x1b[114;5u", "\x1b[27;5;114~"};
    for (size_t i = 0; i < sizeof seqs / sizeof seqs[0]; i++) {
        tui_decoded d;
        size_t n = strlen(seqs[i]);
        for (size_t split = 0; split < n; split++)
            ASSERT_EQ(0, tui_decode_one(seqs[i], split, false, &d));
        ASSERT_EQ(n, tui_decode_one(seqs[i], n, false, &d));
        ASSERT_EQ(TUI_K_DICTATE, d.key);
    }
    PASS();
}

SUITE(dictation_suite) {
    RUN_TEST(dictation_wav_is_complete_pcm_with_duration_bounds);
    RUN_TEST(dictation_wav_rejects_truncated_chunks_and_overflow);
    RUN_TEST(dictation_text_is_bounded_utf8_without_terminal_controls);
    RUN_TEST(dictation_provider_does_not_select_or_rewrite_chat);
    RUN_TEST(dictation_xai_credentials_preserve_chat_and_reject_invalid_flags);
    RUN_TEST(dictation_inserts_at_caret_without_submitting);
    RUN_TEST(dictation_composer_capacity_preserves_the_draft);
    RUN_TEST(dictation_shortcut_decodes_at_every_split);
}
