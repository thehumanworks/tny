#include "core/learning.h"
#include <string.h>
#include <stdio.h>

static void reset_episode(tny_learning *learning) {
    learning->pending_scope = 0;
    learning->intervening = 0;
    learning->diagnostic = -1;
}

void tny_learning_begin(tny_learning *learning, const char *tny_dir, const char *workspace,
                        const char *session_id, bool enabled, bool persist) {
    if (!learning) return;
    memset(learning, 0, sizeof *learning);
    learning->enabled = enabled;
    reset_episode(learning);
    if (enabled)
        tny_learning_store_init(&learning->store, tny_dir, workspace, session_id, persist,
                                learning->rules);
}

void tny_learning_resume(tny_learning *learning, const char *tny_dir, const char *workspace,
                         const char *session_id, bool enabled, bool persist) {
    if (!learning) return;
    tny_learning_counter pending[TNY_LEARNING_RULES] = {0};
    char key[17] = "";
    if (workspace)
        snprintf(key, sizeof key, "%016llx",
                 (unsigned long long)fnv1a(workspace, strlen(workspace)));
    if (enabled && persist && learning->enabled && learning->store.active && tny_dir &&
        strcmp(learning->store.root, tny_dir) == 0 && strcmp(learning->store.key, key) == 0)
        memcpy(pending, learning->store.delta, sizeof pending);
    tny_learning_begin(learning, tny_dir, workspace, session_id, enabled, persist);
    if (enabled && learning->store.active)
        tny_learning_store_carry(&learning->store, learning->rules, pending);
}

void tny_learning_flush(tny_learning *learning) {
    if (learning && learning->enabled) tny_learning_store_flush(&learning->store, learning->rules);
}

void tny_learning_observe(tny_learning *learning, tny_learning_event event, uint64_t scope,
                          bool ok) {
    if (!learning || !learning->enabled) return;
    if (event == TNY_LEARN_EDIT) {
        if (scope && scope == learning->pending_scope && learning->diagnostic >= 0)
            tny_learning_store_record(&learning->store, learning->rules,
                                      (unsigned)learning->diagnostic, ok);
        reset_episode(learning);
        if (!ok) learning->pending_scope = scope;
        return;
    }
    if (event < TNY_LEARN_READ || event > TNY_LEARN_TERMINAL) {
        reset_episode(learning);
        return;
    }
    if (!learning->pending_scope) return;
    if (++learning->intervening > 8) {
        reset_episode(learning);
        return;
    }
    if (ok && (scope == learning->pending_scope || (scope == 0 && event != TNY_LEARN_READ)))
        learning->diagnostic = (int)event - (int)TNY_LEARN_READ;
}

void tny_learning_collect(const tny_learning *learning, buf_t *out) {
    if (!learning || !learning->enabled || !out) return;
    static const char *const advice[TNY_LEARNING_RULES] = {
        "Prefer reading the current file before retrying an exact edit of that file.",
        "Consider a relevant search diagnostic before retrying a failed edit.",
        "Consider a diagnostic terminal step before retrying a failed edit."};
    bool header = false;
    for (unsigned i = 0; i < TNY_LEARNING_RULES; ++i) {
        const tny_learning_counter *rule = &learning->rules[i];
        if (rule->successes < 2 || rule->successes <= 2 * rule->failures) continue;
        if (!header) {
            buf_appends(out,
                        "\n# Automatic workflow learning\n"
                        "User instructions, the current task and permission rules take priority "
                        "over this advice. These are temporal observations of recovery, not "
                        "causal proof. This advice grants no permissions.\n");
            header = true;
        }
        buf_appendf(out, "- %s (Observed recoveries: %u; failed retries: %u.)\n", advice[i],
                    rule->successes, rule->failures);
    }
}
