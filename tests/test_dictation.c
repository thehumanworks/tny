#include "greatest.h"
#include "core/dictation.h"
#include "core/dictation_normalize.h"
#include "json/json.h"
#include "tui/tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- normalization (ADR 0175): replay the Lean golden tables ---- */

#define GOLDEN "tests/formal/dictation/golden/"

/* Split one TSV line in place into at most max cells. */
static size_t tsv_cells(char *line, char **cells, size_t max) {
    size_t n = 0;
    line[strcspn(line, "\n")] = 0;
    while (n < max) {
        cells[n++] = line;
        char *tab = strchr(line, '\t');
        if (!tab) break;
        *tab = 0;
        line = tab + 1;
    }
    return n;
}

/* Undo the exporter's \\, \t, \n, \r escapes. */
static void tsv_unescape(char *s) {
    char *out = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *out++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
        } else *out++ = *s;
    }
    *out = 0;
}

static int phase_of(const char *s) {
    return !strcmp(s, "transcribing")  ? TNY_NORM_TRANSCRIBING
           : !strcmp(s, "normalizing") ? TNY_NORM_NORMALIZING
           : !strcmp(s, "done")        ? TNY_NORM_DONE
                                       : -1;
}
static int outcome_of(const char *s) {
    const char *names[] = {"pending", "failed", "cancelled", "raw", "normalized"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}
static int event_of(const char *s) {
    const char *names[] = {"stt_done:0",      "stt_done:1",  "stt_failed", "cancel",
                           "effort_rejected", "norm_failed", "proposal:0", "proposal:1"};
    for (int i = 0; i < 8; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}
static bool state_of(char **c, tny_norm_state *s) {
    int phase = phase_of(c[0]), outcome = outcome_of(c[4]);
    if (phase < 0 || outcome < 0) return false;
    *s = (tny_norm_state){(tny_norm_phase)phase, c[1][0] == '1', c[2][0] == '1',
                          (unsigned)atoi(c[3]), (tny_norm_outcome)outcome};
    return true;
}

TEST normalize_lifecycle_matches_lean_table(void) {
    FILE *f = fopen(GOLDEN "lifecycle.tsv", "r");
    ASSERT(f);
    char line[512];
    size_t rows = 0;
    ASSERT(fgets(line, sizeof line, f)); /* header */
    while (fgets(line, sizeof line, f)) {
        char *c[11];
        ASSERT_EQ(11, tsv_cells(line, c, 11));
        tny_norm_state s, want;
        ASSERT(state_of(c, &s));
        ASSERT(state_of(c + 6, &want));
        int ev = event_of(c[5]);
        ASSERT(ev >= 0);
        tny_norm_state got = tny_norm_step(s, (tny_norm_event)ev);
        if (got.phase != want.phase || got.enabled != want.enabled || got.effort != want.effort ||
            got.requests != want.requests || got.outcome != want.outcome)
            FAILm(c[5]);
        rows++;
    }
    fclose(f);
    ASSERT_EQ(180u * 8u, rows);
    PASS();
}

static bool golden_dictionary(const char *name, tny_dictionary *out) {
    FILE *f = fopen(GOLDEN "dictionaries.tsv", "r");
    char line[8192], err[160];
    bool found = false;
    while (f && !found && fgets(line, sizeof line, f)) {
        char *c[2];
        if (tsv_cells(line, c, 2) == 2 && !strcmp(c[0], name))
            found = tny_dictionary_parse(c[1], strlen(c[1]), out, err, sizeof err);
    }
    if (f) fclose(f);
    return found;
}

TEST normalize_verifier_matches_lean_table(void) {
    tny_dictionary none, project;
    ASSERT(golden_dictionary("none", &none));
    ASSERT(golden_dictionary("project", &project));
    ASSERT_EQ(0, none.n);
    ASSERT_EQ(6, project.n);
    FILE *f = fopen(GOLDEN "verify.tsv", "r");
    ASSERT(f);
    size_t cap = 1u << 16;
    char *line = malloc(cap);
    ASSERT(line);
    size_t rows = 0, accepted = 0;
    ASSERT(fgets(line, (int)cap, f));
    while (fgets(line, (int)cap, f)) {
        char *c[5];
        ASSERT_EQ(5, tsv_cells(line, c, 5));
        tsv_unescape(c[1]);
        tsv_unescape(c[4]);
        tny_norm_proposal p;
        ASSERT(tny_norm_proposal_parse(c[2], strlen(c[2]), &p));
        const tny_dictionary *d = !strcmp(c[0], "none") ? &none : &project;
        tny_norm_verdict v = tny_norm_verify(d, c[1], strlen(c[1]), &p);
        if (strcmp(tny_norm_verdict_name(v), c[3]) != 0) {
            fprintf(stderr, "verify %s: want %s got %s\n", c[2], c[3], tny_norm_verdict_name(v));
            FAILm("verdict differs from the Lean table");
        }
        /* deliver: the accepted rewrite, otherwise the raw transcript */
        const char *delivered = v == TNY_NORM_VERDICT_OK ? p.text : c[1];
        ASSERT_STR_EQ(c[4], delivered);
        accepted += v == TNY_NORM_VERDICT_OK;
        tny_norm_proposal_free(&p);
        rows++;
    }
    fclose(f);
    free(line);
    tny_dictionary_free(&none);
    tny_dictionary_free(&project);
    ASSERT(rows > 1000);
    ASSERT(accepted > 100);
    PASS();
}

static tny_norm_correction corr(const char *span, const char *repl, tny_norm_reason reason) {
    return (tny_norm_correction){(char *)span, (char *)repl, strlen(span), strlen(repl), reason};
}

static tny_norm_verdict verify1(const tny_dictionary *d, const char *raw, const char *text,
                                tny_norm_correction *cs, size_t n) {
    tny_norm_proposal p = {(char *)text, strlen(text), cs, n};
    return tny_norm_verify(d, raw, strlen(raw), &p);
}

TEST normalize_verifier_rejects_every_unsafe_rewrite(void) {
    const char *json = "{\"tny\":{\"aliases\":[\"tiny\"],\"case\":\"exact\"},\"kubectl\":"
                       "{\"aliases\":[\"kube cuddle\"]}}";
    tny_dictionary d;
    char err[160];
    ASSERT(tny_dictionary_parse(json, strlen(json), &d, err, sizeof err));
    const char *raw = "ask tiny to run kube cuddle on twenty three pods";
    tny_norm_correction ok[] = {corr("tiny", "tny", TNY_NORM_REASON_DICTIONARY),
                                corr("kube cuddle", "kubectl", TNY_NORM_REASON_DICTIONARY),
                                corr("twenty three", "23", TNY_NORM_REASON_NUMBER)};
    ASSERT_EQ(TNY_NORM_VERDICT_OK, verify1(&d, raw, "ask tny to run kubectl on 23 pods", ok, 3));
    /* unlisted edit: an extra word, a dropped word, a silent case change */
    ASSERT_EQ(TNY_NORM_VERDICT_UNLISTED,
              verify1(&d, raw, "ask tny to run kubectl on 23 pods now", ok, 3));
    ASSERT_EQ(TNY_NORM_VERDICT_UNLISTED, verify1(&d, raw, "tny to run kubectl on 23 pods", ok, 3));
    ASSERT_EQ(TNY_NORM_VERDICT_UNLISTED,
              verify1(&d, raw, "Ask tny to run kubectl on 23 pods", ok, 3));
    /* out of order: the cursor has passed the earlier span */
    tny_norm_correction swapped[] = {ok[1], ok[0]};
    ASSERT_EQ(TNY_NORM_VERDICT_UNLISTED,
              verify1(&d, raw, "ask tny to run kubectl on twenty three pods", swapped, 2));
    /* non-dictionary replacement, wrong exact case, multi-word span without alias */
    tny_norm_correction bad[] = {corr("ask", "delete", TNY_NORM_REASON_DICTIONARY)};
    ASSERT_EQ(TNY_NORM_VERDICT_INADMISSIBLE,
              verify1(&d, raw, "delete tiny to run kube cuddle on twenty three pods", bad, 1));
    bad[0] = corr("tiny", "TNY", TNY_NORM_REASON_DICTIONARY);
    ASSERT_EQ(TNY_NORM_VERDICT_INADMISSIBLE,
              verify1(&d, raw, "ask TNY to run kube cuddle on twenty three pods", bad, 1));
    bad[0] = corr("ask tiny", "tny", TNY_NORM_REASON_DICTIONARY);
    ASSERT_EQ(TNY_NORM_VERDICT_INADMISSIBLE,
              verify1(&d, raw, "tny to run kube cuddle on twenty three pods", bad, 1));
    bad[0] = corr("twenty three", "24", TNY_NORM_REASON_NUMBER);
    ASSERT_EQ(TNY_NORM_VERDICT_INADMISSIBLE,
              verify1(&d, raw, "ask tiny to run kube cuddle on 24 pods", bad, 1));
    bad[0] = corr("pods", "Pods!", TNY_NORM_REASON_CASE);
    ASSERT_EQ(TNY_NORM_VERDICT_INADMISSIBLE,
              verify1(&d, raw, "ask tiny to run kube cuddle on twenty three Pods!", bad, 1));
    bad[0] = corr("", "x", TNY_NORM_REASON_CASE);
    ASSERT_EQ(TNY_NORM_VERDICT_EMPTY_SPAN, verify1(&d, raw, raw, bad, 1));
    /* invalid text never passes, even when "listed" */
    ASSERT_EQ(TNY_NORM_VERDICT_INVALID_TEXT, verify1(&d, raw, "", NULL, 0));
    ASSERT_EQ(TNY_NORM_VERDICT_INVALID_TEXT, verify1(&d, raw, "a\x1b[2J", NULL, 0));
    /* oversized diff: every word swapped for a dictionary word */
    const char *many = "tiny tiny tiny tiny tiny";
    tny_norm_correction five[5];
    for (int i = 0; i < 5; i++) five[i] = corr("tiny", "tny", TNY_NORM_REASON_DICTIONARY);
    ASSERT_EQ(TNY_NORM_VERDICT_EDIT_BOUND, verify1(&d, many, "tny tny tny tny tny", five, 5));
    ASSERT_EQ(TNY_NORM_VERDICT_OK, verify1(&d, many, "tny tny tny tny tiny", five, 4));
    /* the unchanged transcript is always accepted */
    ASSERT_EQ(TNY_NORM_VERDICT_OK, verify1(&d, raw, raw, NULL, 0));
    tny_norm_correction lots[TNY_NORMALIZE_CORRECTIONS_MAX + 1];
    for (size_t i = 0; i < TNY_NORMALIZE_CORRECTIONS_MAX + 1; i++)
        lots[i] = corr("x", "x", TNY_NORM_REASON_CASE);
    ASSERT_EQ(TNY_NORM_VERDICT_TOO_MANY,
              verify1(&d, raw, raw, lots, TNY_NORMALIZE_CORRECTIONS_MAX + 1));
    tny_dictionary_free(&d);
    PASS();
}

TEST normalize_numbers_accept_only_exact_cardinals(void) {
    struct {
        const char *s;
        bool ok;
        uint64_t v;
    } cases[] = {{"zero", true, 0},
                 {"twenty-three,", true, 23},
                 {"One Hundred and Five", true, 105},
                 {"two million forty", true, 2000040},
                 {"nine hundred ninety nine billion", true, 999000000000ULL},
                 {"1,234,567", true, 1234567},
                 {"123,456", true, 123456},
                 {"1234,", true, 1234},
                 {"100000000000000", true, 100000000000000ULL},
                 {"999,999,999,999,999", true, 999999999999999ULL},
                 {"0", true, 0},
                 {"five three", false, 0},
                 {"twenty thirteen", false, 0},
                 {"one thousand and five", false, 0},
                 {"one thousand two thousand", false, 0},
                 {"zero thousand", false, 0},
                 {"007", false, 0},
                 {"1,23", false, 0},
                 {"12,345,", true, 12345},
                 {"12,34,567", false, 0},
                 {"0,123", false, 0},
                 {"01,234", false, 0},
                 {"1234,567", false, 0},
                 {"1234567890123456", false, 0},
                 {"minus five", false, 0},
                 {"", false, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint64_t v = 99;
        bool ok = tny_norm_number_value(cases[i].s, strlen(cases[i].s), &v);
        if (ok != cases[i].ok || (ok && v != cases[i].v)) FAILm(cases[i].s);
    }
    PASS();
}

/* Reference Levenshtein over whitespace words (already normalized). */
static size_t naive_lev(char **a, size_t n, char **b, size_t m) {
    size_t *row = calloc(m + 1, sizeof *row);
    if (!row) abort();
    for (size_t j = 0; j <= m; j++) row[j] = j;
    for (size_t i = 1; i <= n; i++) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= m; j++) {
            size_t up = row[j];
            size_t v = diag + (strcmp(a[i - 1], b[j - 1]) != 0);
            if (up + 1 < v) v = up + 1;
            if (row[j - 1] + 1 < v) v = row[j - 1] + 1;
            row[j] = v;
            diag = up;
        }
    }
    size_t d = row[m];
    free(row);
    return d;
}

TEST normalize_edit_bound_counts_insertions_into_empty_meaning(void) {
    /* No semantic raw tokens: n = 0, so at most three inserted words. */
    ASSERT_EQ(1, tny_norm_within_edit_bound("...", 3, "... a", 5));
    ASSERT_EQ(1, tny_norm_within_edit_bound("...", 3, "a b c", 5));
    ASSERT_EQ(0, tny_norm_within_edit_bound("...", 3, "a b c d", 7));
    ASSERT_EQ(1, tny_norm_within_edit_bound("a b c d", 7, "a b c d e f g h", 15));
    ASSERT_EQ(0, tny_norm_within_edit_bound("a b c d", 7, "a b c d e f g h i", 17));
    PASS();
}

TEST normalize_banded_distance_matches_full_dp(void) {
    static char *words[] = {"a", "b", "c", "d"};
    srand(192);
    for (int round = 0; round < 2000; round++) {
        size_t n = (size_t)(rand() % 24), m = (size_t)(rand() % 24);
        char *a[24], *b[24];
        buf_t sa = {0}, sb = {0};
        for (size_t i = 0; i < n; i++) {
            a[i] = words[rand() % 4];
            buf_appendf(&sa, "%s%s", i ? " " : "", a[i]);
        }
        for (size_t j = 0; j < m; j++) {
            b[j] = words[rand() % 4];
            buf_appendf(&sb, "%s%s", j ? " " : "", b[j]);
        }
        size_t d = naive_lev(a, n, b, m);
        int within = tny_norm_within_edit_bound(sa.data ? sa.data : "", sa.len,
                                                sb.data ? sb.data : "", sb.len);
        buf_free(&sa);
        buf_free(&sb);
        ASSERT_EQ(d <= 3 + n / 4, within == 1);
    }
    PASS();
}

TEST normalize_proposal_parse_is_strict(void) {
    const char *good = "{\"text\":\"a\",\"corrections\":[{\"span\":\"x\",\"replacement\":\"y\","
                       "\"reason\":\"case\"}]}";
    tny_norm_proposal p;
    ASSERT(tny_norm_proposal_parse(good, strlen(good), &p));
    ASSERT_EQ(1, p.n);
    ASSERT_EQ(TNY_NORM_REASON_CASE, p.corrections[0].reason);
    tny_norm_proposal_free(&p);
    const char *bad[] = {
        "",
        "```json\n{\"text\":\"a\",\"corrections\":[]}\n```",
        "{\"text\":\"a\"}",
        "{\"text\":\"a\",\"corrections\":[],\"note\":1}",
        "{\"text\":1,\"corrections\":[]}",
        "{\"text\":\"a\\u0000b\",\"corrections\":[]}",
        "{\"text\":\"a\",\"corrections\":{}}",
        "{\"text\":\"a\",\"corrections\":[{\"span\":\"x\",\"replacement\":\"y\"}]}",
        "{\"text\":\"a\",\"corrections\":[{\"span\":\"x\",\"replacement\":\"y\",\"reason\":"
        "\"style\"}]}",
        "{\"text\":\"a\",\"corrections\":[{\"span\":\"x\",\"replacement\":2,\"reason\":\"case\"}]}",
        "{\"text\":\"a\",\"corrections\":[{\"span\":\"x\",\"replacement\":\"y\",\"reason\":"
        "\"case\","
        "\"why\":\"\"}]}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ASSERT(!tny_norm_proposal_parse(bad[i], strlen(bad[i]), &p));
        ASSERT_EQ(0, p.n);
        ASSERT(!p.text);
    }
    PASS();
}

TEST normalize_dictionary_parse_validates_and_bounds(void) {
    tny_dictionary d;
    char err[160];
    const char *good = "{\"$schema\":\"https://example.invalid/s.json\",\"tny\":\"the harness\","
                       "\"Jev\":{\"context\":\"engine\",\"aliases\":[\"jeff\"],\"case\":\"exact\"},"
                       "\"C++\":{}}";
    ASSERT(tny_dictionary_parse(good, strlen(good), &d, err, sizeof err));
    ASSERT_EQ(3, d.n);
    ASSERT_STR_EQ("tny", d.entries[0].word);
    ASSERT_STR_EQ("the harness", d.entries[0].context);
    ASSERT(!d.entries[0].exact);
    ASSERT(d.entries[1].exact);
    ASSERT_EQ(1, d.entries[1].n_aliases);
    tny_dictionary_free(&d);
    const char *bad[] = {
        "[]",
        "{\"\":\"x\"}",
        "{\"...\":\"punctuation only\"}",
        "{\" padded\":\"x\"}",
        "{\"tny\":1}",
        "{\"tny\":\"bad\\u001bcontext\"}",
        "{\"tny\":{\"aliases\":[\"\"]}}",
        "{\"tny\":{\"aliases\":\"tiny\"}}",
        "{\"tny\":{\"case\":\"upper\"}}",
        "{\"tny\":{\"weight\":1}}",
        "{\"tny\":\"a\",\"tny\":\"b\"}",
        "{\"tny\":{\"aliases\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}}",
        "{\"tny\":\"x\""};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (tny_dictionary_parse(bad[i], strlen(bad[i]), &d, err, sizeof err)) FAILm(bad[i]);
        ASSERT_EQ(0, d.n);
        ASSERT(*err);
    }
    /* word and entry-count limits */
    buf_t b = {0};
    buf_appends(&b, "{\"");
    for (int i = 0; i < 65; i++) buf_appends(&b, "w");
    buf_appends(&b, "\":\"x\"}");
    ASSERT(!tny_dictionary_parse(b.data, b.len, &d, err, sizeof err));
    buf_clear(&b);
    buf_appends(&b, "{");
    for (int i = 0; i < 257; i++) buf_appendf(&b, "%s\"w%d\":\"\"", i ? "," : "", i);
    buf_appends(&b, "}");
    ASSERT(!tny_dictionary_parse(b.data, b.len, &d, err, sizeof err));
    ASSERT(strstr(err, "256"));
    buf_free(&b);
    PASS();
}

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) abort();
    fputs(data, f);
    fclose(f);
}

TEST normalize_dictionary_project_wins_per_word(void) {
    char home[] = "/tmp/tny-dict-XXXXXX";
    ASSERT(mkdtemp(home));
    char *old = getenv("HOME") ? xstrdup(getenv("HOME")) : NULL;
    setenv("HOME", home, 1);
    char path[512], ws[512];
    snprintf(path, sizeof path, "%s/.tny", home);
    mkdir(path, 0700);
    snprintf(ws, sizeof ws, "%s/ws", home);
    mkdir(ws, 0700);
    snprintf(path, sizeof path, "%s/ws/.tny", home);
    mkdir(path, 0700);
    tny_dictionary d;
    char err[256] = "";
    /* both missing: empty, not an error */
    ASSERT(tny_dictionary_load(ws, &d, err, sizeof err));
    ASSERT_EQ(0, d.n);
    snprintf(path, sizeof path, "%s/.tny/dictionary.json", home);
    write_file(path, "{\"tny\":\"user context\",\"Jev\":\"engine\"}");
    snprintf(path, sizeof path, "%s/ws/.tny/dictionary.json", home);
    write_file(path,
               "{\"tny\":{\"context\":\"project context\",\"case\":\"exact\"},\"kubectl\":\"k\"}");
    ASSERT(tny_dictionary_load(ws, &d, err, sizeof err));
    ASSERT_EQ(3, d.n);
    ASSERT_STR_EQ("tny", d.entries[0].word);
    ASSERT_STR_EQ("project context", d.entries[0].context);
    ASSERT(d.entries[0].exact);
    ASSERT_STR_EQ("kubectl", d.entries[1].word);
    ASSERT_STR_EQ("Jev", d.entries[2].word);
    tny_dictionary_free(&d);
    /* one invalid file fails the whole load (normalization then keeps raw) */
    write_file(path, "{\"tny\":");
    ASSERT(!tny_dictionary_load(ws, &d, err, sizeof err));
    ASSERT(strstr(err, "dictionary.json"));
    ASSERT_EQ(0, d.n);
    unlink(path);
    snprintf(path, sizeof path, "%s/.tny/dictionary.json", home);
    unlink(path);
    snprintf(path, sizeof path, "%s/ws/.tny", home);
    rmdir(path);
    rmdir(ws);
    snprintf(path, sizeof path, "%s/.tny", home);
    rmdir(path);
    rmdir(home);
    if (old) setenv("HOME", old, 1);
    else unsetenv("HOME");
    free(old);
    PASS();
}

TEST normalize_config_is_on_by_default_and_explicit(void) {
    tny_ctx ctx = {0};
    tny_norm_config c;
    char err[160];
    unsetenv("TNY_DICTATION_NORMALIZE");
    unsetenv("TNY_DICTATION_NORMALIZE_MODEL");
    unsetenv("TNY_DICTATION_NORMALIZE_EFFORT");
    const char *json = "{\"dictation\":{\"normalize\":{\"model\":{\"xai\":\"grok-x\"},"
                       "\"fast\":true,\"timeout_seconds\":7}}}";
    ctx.settings = jparse(json, strlen(json));
    ASSERT(ctx.settings);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(c.enabled && c.valid); /* object without enabled inherits the default */
    tny_norm_config_resolve(&ctx, "xai", TNY_DICTATION_NORMALIZE_ON, &c, err, sizeof err);
    ASSERT(c.enabled && c.valid);
    ASSERT_STR_EQ("grok-x", c.model);
    ASSERT_STR_EQ("none", c.effort); /* canonical off on the OpenAI wire */
    ASSERT(c.fast);
    ASSERT_EQ(7, c.timeout_seconds);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_ON, &c, err, sizeof err);
    ASSERT_STR_EQ("", c.model); /* the adapter default applies */
    yyjson_doc_free(ctx.settings);
    ctx.settings = jparse("{}", 2);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(c.enabled && c.valid); /* no dictation settings */
    yyjson_doc_free(ctx.settings);
    json = "{\"dictation\":{\"normalize\":{\"enabled\":false}}}";
    ctx.settings = jparse(json, strlen(json));
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(!c.enabled);
    setenv("TNY_DICTATION_NORMALIZE", "1", 1);
    setenv("TNY_DICTATION_NORMALIZE_EFFORT", "omit", 1);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(c.enabled && c.valid);
    ASSERT_STR_EQ("", c.effort);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_OFF, &c, err, sizeof err);
    ASSERT(!c.enabled);
    setenv("TNY_DICTATION_NORMALIZE", "0", 1);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_ON, &c, err, sizeof err);
    ASSERT(c.enabled); /* explicit CLI flag wins */
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(!c.enabled);
    setenv("TNY_DICTATION_NORMALIZE", "yes", 1);
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(!c.valid);
    unsetenv("TNY_DICTATION_NORMALIZE");
    unsetenv("TNY_DICTATION_NORMALIZE_EFFORT");
    yyjson_doc_free(ctx.settings);
    json = "{\"dictation\":{\"normalize\":{\"enabled\":true,\"timeout_seconds\":0}}}";
    ctx.settings = jparse(json, strlen(json));
    tny_norm_config_resolve(&ctx, "codex", TNY_DICTATION_NORMALIZE_DEFAULT, &c, err, sizeof err);
    ASSERT(c.enabled && !c.valid);
    ASSERT(strstr(err, "timeout_seconds"));
    yyjson_doc_free(ctx.settings);
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
    RUN_TEST(normalize_lifecycle_matches_lean_table);
    RUN_TEST(normalize_verifier_matches_lean_table);
    RUN_TEST(normalize_verifier_rejects_every_unsafe_rewrite);
    RUN_TEST(normalize_numbers_accept_only_exact_cardinals);
    RUN_TEST(normalize_edit_bound_counts_insertions_into_empty_meaning);
    RUN_TEST(normalize_banded_distance_matches_full_dp);
    RUN_TEST(normalize_proposal_parse_is_strict);
    RUN_TEST(normalize_dictionary_parse_validates_and_bounds);
    RUN_TEST(normalize_dictionary_project_wins_per_word);
    RUN_TEST(normalize_config_is_on_by_default_and_explicit);
}
