/* Prompt retention drives the production history implementation, including
 * its disk format and allocator failures. No replacement history model. */
#include "greatest.h"
#include "tui/tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool fail_copy, fail_array;

static char *history_copy(const char *s) { return fail_copy ? NULL : xstrdup(s); }
static void *history_realloc(void *p, size_t n) { return fail_array ? NULL : realloc(p, n); }

/* Fault injection stays in this translation unit, around the real source. */
static void fault_hist_load(tui *t);
static void fault_hist_add(tui *t, const char *line);
static void fault_hist_free(tui *t);
#define tui_hist_load fault_hist_load
#define tui_hist_add  fault_hist_add
#define tui_hist_free fault_hist_free
#define xstrdup       history_copy
#define realloc       history_realloc
#include "../src/tui/tui_hist.c"
#undef realloc
#undef xstrdup
#undef tui_hist_free
#undef tui_hist_add
#undef tui_hist_load

static char root[512], directory[540], history[560];
static char *old_home;
static tny_ctx ctx;
static tui state;

static void setup(void *ud) {
    (void)ud;
    memset(&ctx, 0, sizeof ctx);
    memset(&state, 0, sizeof state);
    state.ctx = &ctx;
    fail_copy = fail_array = false;
    const char *value = getenv("HOME");
    old_home = value ? xstrdup(value) : NULL;
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/tny-history-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) abort();
    if (setenv("HOME", root, 1) != 0) abort();
    snprintf(directory, sizeof directory, "%s/.tny", root);
    snprintf(history, sizeof history, "%s/history", directory);
}

static void teardown(void *ud) {
    (void)ud;
    fault_hist_free(&state);
    buf_free(&state.out);
    buf_free(&state.transcript);
    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
    free(old_home);
    old_home = NULL;
    unlink(history);
    rmdir(directory);
    rmdir(root);
}

TEST prompt_history_retains_the_latest_entries_without_writing_ephemeral_state(void) {
    ctx.no_save = true;
    char prompt[32];
    for (int i = 0; i < TUI_MAX_HIST * 3; i++) {
        snprintf(prompt, sizeof prompt, "prompt %d", i);
        tui_hist_add(&state, prompt);
        ASSERT(state.n_hist <= TUI_MAX_HIST);
    }
    ASSERT_EQ(TUI_MAX_HIST, state.n_hist);
    ASSERT_EQ(state.n_hist, state.hist_pos);
    for (int i = 0; i < state.n_hist; i++) {
        snprintf(prompt, sizeof prompt, "prompt %d", TUI_MAX_HIST * 2 + i);
        ASSERT_STR_EQ(prompt, state.hist[i]);
    }
    state.hist_pos = 0;
    tui_hist_add(&state, prompt); /* duplicate newest prompt never evicts */
    ASSERT_EQ(TUI_MAX_HIST, state.n_hist);
    ASSERT_EQ(state.n_hist, state.hist_pos);
    tui_hist_add(&state, NULL);
    tui_hist_add(&state, "");
    ASSERT_EQ(TUI_MAX_HIST, state.n_hist);
    ASSERT(access(history, F_OK) != 0);
    PASS();
}

TEST prompt_history_streams_saved_records_without_truncating_the_disk_log(void) {
    ASSERT_EQ(0, mkdir_p(directory));
    FILE *f = fopen(history, "w");
    ASSERT(f);
    for (int i = 0; i < TUI_MAX_HIST + 17; i++) fprintf(f, "prompt %d\n", i);
    fputs("\nmultiline\\nwith \\\\ slash\ntail without newline", f);
    ASSERT_EQ(0, fclose(f));
    size_t before_len = 0, after_len = 0;
    char *before = file_slurp(history, &before_len);
    ASSERT(before);
    tui_hist_load(&state);
    ASSERT_EQ(TUI_MAX_HIST, state.n_hist);
    ASSERT_EQ(state.n_hist, state.hist_pos);
    ASSERT_STR_EQ("prompt 19", state.hist[0]);
    ASSERT_STR_EQ("multiline\nwith \\ slash", state.hist[TUI_MAX_HIST - 2]);
    ASSERT_STR_EQ("tail without newline", state.hist[TUI_MAX_HIST - 1]);
    char *after = file_slurp(history, &after_len);
    ASSERT(after);
    ASSERT_EQ(before_len, after_len);
    ASSERT_MEM_EQ(before, after, before_len);
    free(after);

    tui_hist_add(&state, "new\nback\\slash");
    ASSERT_EQ(TUI_MAX_HIST, state.n_hist);
    ASSERT_STR_EQ("prompt 20", state.hist[0]);
    ASSERT_STR_EQ("new\nback\\slash", state.hist[TUI_MAX_HIST - 1]);
    after = file_slurp(history, &after_len);
    ASSERT(after);
    ASSERT(after_len > before_len);
    ASSERT_MEM_EQ(before, after, before_len);
    ASSERT_STR_EQ("new\\nback\\\\slash\n", after + before_len);
    free(after);
    free(before);
    PASS();
}

TEST prompt_history_readonly_and_background_views_keep_disk_bytes_unchanged(void) {
    tui_hist_add(&state, "saved");
    state.session_readonly = true;
    tui_hist_add(&state, "readonly prompt");
    state.session_readonly = false;
    state.background_view = true;
    tui_hist_add(&state, "background prompt");
    ASSERT_EQ(3, state.n_hist);
    char *saved = file_slurp(history, NULL);
    ASSERT(saved);
    ASSERT_STR_EQ("saved\n", saved);
    free(saved);
    PASS();
}

TEST prompt_history_allocation_failures_preserve_valid_retained_entries(void) {
    ctx.no_save = true;
    fail_copy = true;
    fault_hist_add(&state, "cannot copy");
    ASSERT_EQ(0, state.n_hist);
    ASSERT_EQ(NULL, state.hist);
    fail_copy = false;
    tui_hist_add(&state, "retained");
    fail_array = true;
    fault_hist_add(&state, "cannot grow");
    ASSERT_EQ(1, state.n_hist);
    ASSERT_EQ(1, state.hist_pos);
    ASSERT_STR_EQ("retained", state.hist[0]);
    fail_array = false;
    for (int i = 1; i < TUI_MAX_HIST; i++) {
        char prompt[32];
        snprintf(prompt, sizeof prompt, "prompt %d", i);
        tui_hist_add(&state, prompt);
    }
    fail_copy = true;
    fault_hist_add(&state, "must not evict on failure");
    ASSERT_EQ(TUI_MAX_HIST, state.n_hist);
    ASSERT_STR_EQ("retained", state.hist[0]);
    ASSERT_STR_EQ("prompt 499", state.hist[TUI_MAX_HIST - 1]);
    PASS();
}

TEST prompt_history_load_drops_failed_allocations_without_storing_null_entries(void) {
    ASSERT_EQ(0, mkdir_p(directory));
    ASSERT_EQ(0, file_write_atomic(history, "one\ntwo\n", 8));
    fail_array = true;
    fault_hist_load(&state);
    ASSERT_EQ(0, state.n_hist);
    ASSERT_EQ(NULL, state.hist);
    fail_array = false;
    fault_hist_load(&state);
    ASSERT_EQ(2, state.n_hist);
    ASSERT_STR_EQ("one", state.hist[0]);
    ASSERT_STR_EQ("two", state.hist[1]);
    PASS();
}

SUITE(tui_hist_suite) {
    SET_SETUP(setup, NULL);
    SET_TEARDOWN(teardown, NULL);
    RUN_TEST(prompt_history_retains_the_latest_entries_without_writing_ephemeral_state);
    RUN_TEST(prompt_history_streams_saved_records_without_truncating_the_disk_log);
    RUN_TEST(prompt_history_readonly_and_background_views_keep_disk_bytes_unchanged);
    RUN_TEST(prompt_history_allocation_failures_preserve_valid_retained_entries);
    RUN_TEST(prompt_history_load_drops_failed_allocations_without_storing_null_entries);
}
