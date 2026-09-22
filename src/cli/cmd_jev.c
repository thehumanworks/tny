/* Thin CLI adapter: no chat context, credential refresh, tools or sessions. */
#include "cli/cli.h"
#include "core/tnyjev.h"
#include "json/json.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
}
static bool cancelled(void *ud) {
    (void)ud;
    return interrupted != 0;
}

static bool read_state(buf_t *state) {
    while (!interrupted) {
        struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
        int ready = tny_poll(&fd, 1, 50);
        if (ready < 0 && errno != EINTR) return false;
        if (ready <= 0) continue;
        char bytes[8192];
        ssize_t n = read(STDIN_FILENO, bytes, sizeof bytes);
        if (n == 0) return state->len && utf8_valid_bytes(state->data, state->len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if ((size_t)n > TNYJEV_MAX_BYTES - state->len) return false;
        buf_append(state, bytes, (size_t)n);
        if (state->oom) return false;
    }
    return false;
}

static void render_result(buf_t *out, bool json, const tnyjev_request *r,
                          const tnyjev_result *result) {
    bool score = r->kind == TNYJEV_SCORE;
    if (!json) {
        if (score) buf_appendf(out, "%.17g\n", result->value.score);
        else buf_appendf(out, "%s\n", r->choices[result->value.choose.choice_index].key);
        return;
    }
    buf_appends(out, score ? "{\"kind\":\"score\",\"primitive\":\"noul\",\"score\":"
                           : "{\"kind\":\"choose\",\"primitive\":\"choice\",\"choice\":");
    if (score) buf_appendf(out, "%.17g", result->value.score);
    else {
        jescape(out, r->choices[result->value.choose.choice_index].key);
        buf_appendf(out, ",\"confidence\":%.17g,\"probabilities\":{",
                    result->value.choose.confidence);
        for (size_t i = 0; i < r->choice_count; i++) {
            if (i) buf_appends(out, ",");
            jescape(out, r->choices[i].key);
            buf_appendf(out, ":%.17g", result->value.choose.probabilities[i]);
        }
        buf_appends(out, "}");
    }
    buf_appends(out, ",\"model\":");
    jescape(out, result->model);
    buf_appendf(out, ",\"usage\":{\"input_tokens\":%llu,\"output_tokens\":%llu}}\n",
                (unsigned long long)result->input_tokens,
                (unsigned long long)result->output_tokens);
}

int cmd_jev(const cli_globals *g, int argc, char **argv, bool choose) {
    const char *name = choose ? "choose" : "score";
    tnyjev_request r = {.kind = choose ? TNYJEV_CHOOSE : TNYJEV_SCORE};
    tnyjev_config c = {.api_key = getenv("TYPESAFE_API_KEY"),
                       .url = getenv("TNY_JEV_URL"),
                       .model = g->model ? g->model : getenv("TNY_JEV_MODEL"),
                       .cancelled = cancelled};
    const char *choices_json = NULL;
    bool json = g->json, stdin_state = false, literal = false, state_set = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!literal && (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)) {
            help_for(name);
            return 0;
        }
        if (!literal && strcmp(a, "--") == 0) {
            literal = true;
            continue;
        }
        if (!literal && strcmp(a, "--json") == 0) {
            json = true;
            continue;
        }
        if (!literal && (strcmp(a, "--stdin") == 0 || strcmp(a, "--stdin-json") == 0)) {
            if (state_set) goto usage;
            state_set = stdin_state = true;
            r.state.type = !strcmp(a, "--stdin-json") ? TNYJEV_JSON : TNYJEV_TEXT;
            continue;
        }
        if (!literal && a[0] == '-') {
            if (i + 1 >= argc) goto usage;
            const char *v = argv[++i];
            if (strcmp(a, "--state") == 0 || strcmp(a, "--state-json") == 0) {
                if (state_set) goto usage;
                state_set = true;
                r.state = (tnyjev_value){!strcmp(a, "--state-json") ? TNYJEV_JSON : TNYJEV_TEXT, v};
            } else if (strcmp(a, "--choices") == 0) {
                if (!choose || choices_json) goto usage;
                choices_json = v;
            } else if (strcmp(a, "--model") == 0) {
                c.model = v;
            } else if (strcmp(a, "--timeout") == 0) {
                if (!*v || strspn(v, "0123456789") != strlen(v)) goto usage;
                errno = 0;
                unsigned long seconds = strtoul(v, NULL, 10);
                if (errno || !seconds || seconds > 300) goto usage;
                c.timeout_ms = (unsigned)(seconds * 1000);
            } else goto usage;
            continue;
        }
        if (r.instructions) goto usage;
        r.instructions = a;
    }
    if (!r.instructions && choose) r.instructions = "Which option best matches the state?";
    if (!r.instructions || !*r.instructions || (choose && !choices_json)) goto usage;
    if (!c.api_key || !*c.api_key) {
        fprintf(stderr, "tny: %s: set TYPESAFE_API_KEY to use Jev\n", name);
        return 1;
    }
    if (!state_set) stdin_state = true;
    if (stdin_state && isatty(STDIN_FILENO)) goto usage;

    int rc = 1;
    char error[256] = "invalid state or choices (see command --help)";
    tnyjev_choice choices[TNYJEV_MAX_CHOICES] = {0};
    yyjson_doc *doc = NULL;
    buf_t state = {0}, output = {0};
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    if (choose) {
        size_t len = strlen(choices_json);
        if (len > TNYJEV_MAX_BYTES) goto done;
        doc = jparse(choices_json, len);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        size_t count = yyjson_obj_size(root);
        if (!yyjson_is_obj(root) || !count || count > TNYJEV_MAX_CHOICES) goto done;
        size_t i, n;
        yyjson_val *key, *val;
        r.choices = choices;
        r.choice_count = count;
        yyjson_obj_foreach(root, i, n, key, val) {
            choices[i].key = yyjson_get_str(key);
            if (strlen(choices[i].key) != yyjson_get_len(key)) goto done;
            choices[i].description.type = TNYJEV_JSON;
            choices[i].description.data = jwrite_val(val);
            if (!choices[i].description.data) goto done;
        }
    }
    if (stdin_state) {
        if (!read_state(&state)) goto done;
        r.state.data = state.data;
    }
    tnyjev_result result;
    tnyjev_status status = tnyjev_evaluate(&c, &r, &result, error, sizeof error);
    if (status != TNYJEV_OK) goto done;
    render_result(&output, json, &r, &result);
    if (output.oom || fwrite(output.data, 1, output.len, stdout) != output.len || fflush(stdout)) {
        snprintf(error, sizeof error, "cannot write Jev result");
        goto done;
    }
    rc = 0;
done:
    if (interrupted) {
        rc = 130;
        snprintf(error, sizeof error, "Jev request interrupted");
    }
    if (rc) fprintf(stderr, "tny: %s: %s\n", name, error);
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    for (size_t i = 0; i < TNYJEV_MAX_CHOICES; i++) free((char *)choices[i].description.data);
    yyjson_doc_free(doc);
    if (state.data) secure_zero(state.data, state.len);
    buf_free(&state);
    buf_free(&output);
    return rc;
usage:
    fprintf(stderr, "tny: %s: invalid options; supply state and %s (see tny %s --help)\n", name,
            choose ? "--choices JSON" : "a yes/no question", name);
    return 1;
}
