/* The model proposes, C disposes: the deterministic check of a normalizer
 * rewrite and the normalization lifecycle. Each function is a clause-by-clause
 * translation of tests/formal/dictation (Lean 4, ADR 0175); tests/test_dictation.c
 * replays the golden tables exported from the proven definitions. */
#include "core/dictation_normalize.h"
#include "json/json.h"
#include <stdlib.h>
#include <string.h>

/* ---- text classes (Lean: Dictation.Text) ---- */

static bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_punct(unsigned char c) {
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) ||
           (c >= 123 && c <= 126);
}
static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool is_edge(unsigned char c) { return c && strchr(".,;:!?\"'()[]{}", c) != NULL; }
static bool is_edge_or_space(unsigned char c) { return is_edge(c) || is_space(c); }
static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

typedef struct {
    const char *s;
    size_t n;
} span_t;

static span_t trim_by(span_t t, bool (*p)(unsigned char)) {
    while (t.n && p((unsigned char)t.s[0])) {
        t.s++;
        t.n--;
    }
    while (t.n && p((unsigned char)t.s[t.n - 1])) t.n--;
    return t;
}

static bool folded_equal(span_t a, span_t b) {
    if (a.n != b.n) return false;
    for (size_t i = 0; i < a.n; i++)
        if (lower((unsigned char)a.s[i]) != lower((unsigned char)b.s[i])) return false;
    return true;
}

static bool bytes_equal(span_t a, span_t b) { return a.n == b.n && !memcmp(a.s, b.s, a.n); }

/* Next whitespace-delimited token at or after *at; false at the end. */
static bool next_token(span_t s, size_t *at, span_t *tok) {
    size_t i = *at;
    while (i < s.n && is_space((unsigned char)s.s[i])) i++;
    if (i == s.n) return false;
    size_t start = i;
    while (i < s.n && !is_space((unsigned char)s.s[i])) i++;
    *tok = (span_t){s.s + start, i - start};
    *at = i;
    return true;
}

static size_t token_count(span_t s) {
    size_t at = 0, n = 0;
    span_t tok;
    while (next_token(s, &at, &tok)) n++;
    return n;
}

/* Semantic tokens: bytes in one buffer, token boundaries in `ends`. */
typedef struct {
    buf_t bytes;
    size_t *ends;
    size_t n, cap;
    bool oom;
} sem_tokens;

static void sem_free(sem_tokens *t) {
    buf_free(&t->bytes);
    free(t->ends);
    memset(t, 0, sizeof *t);
}

static span_t sem_at(const sem_tokens *t, size_t i) {
    size_t start = i ? t->ends[i - 1] : 0;
    return (span_t){t->bytes.data + start, t->ends[i] - start};
}

static void sem_build(span_t s, sem_tokens *out) {
    memset(out, 0, sizeof *out);
    size_t at = 0;
    span_t tok;
    while (!out->oom && next_token(s, &at, &tok)) {
        size_t before = out->bytes.len;
        bool digit = false;
        for (size_t i = 0; i < tok.n; i++) digit = digit || is_digit((unsigned char)tok.s[i]);
        if (digit) tok = trim_by(tok, is_edge);
        for (size_t i = 0; i < tok.n; i++) {
            unsigned char c = (unsigned char)tok.s[i];
            if (digit || !is_punct(c)) {
                char folded = (char)lower(c);
                buf_append(&out->bytes, &folded, 1);
            }
        }
        if (out->bytes.oom) out->oom = true;
        if (out->bytes.len == before || out->oom) continue;
        if (out->n == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 16;
            size_t *ends = realloc(out->ends, cap * sizeof *ends);
            if (!ends) {
                out->oom = true;
                break;
            }
            out->ends = ends;
            out->cap = cap;
        }
        out->ends[out->n++] = out->bytes.len;
    }
}

/* 1 equal, 0 different, -1 out of memory. */
static int sem_equal(span_t a, span_t b) {
    sem_tokens x, y;
    sem_build(a, &x);
    sem_build(b, &y);
    int rc = x.oom || y.oom ? -1 : x.n == y.n;
    for (size_t i = 0; rc == 1 && i < x.n; i++) rc = bytes_equal(sem_at(&x, i), sem_at(&y, i));
    sem_free(&x);
    sem_free(&y);
    return rc;
}

static int sem_nonempty(span_t a) {
    sem_tokens x;
    sem_build(a, &x);
    int rc = x.oom ? -1 : x.n > 0;
    sem_free(&x);
    return rc;
}

/* ---- numbers (Lean: numeralVal, wordsVal, numberVal) ---- */

/* One group: nonempty digits, no leading zero unless alone. Several groups:
 * `d{1,3}(,ddd)+` without a leading zero. At most 15 digits either way. */
static bool numeral_value(span_t s, uint64_t *value) {
    const char *comma = s.n ? memchr(s.s, ',', s.n) : NULL;
    size_t first = comma ? (size_t)(comma - s.s) : s.n;
    if (!first) return false;
    if (comma ? first > 3 || s.s[0] == '0' : first > 1 && s.s[0] == '0') return false;
    uint64_t v = 0;
    size_t digits = 0, run = 0;
    for (size_t i = 0; i <= s.n; i++) {
        if (i == s.n || s.s[i] == ',') {
            if (i > first && run != 3) return false;
            run = 0;
            continue;
        }
        if (!is_digit((unsigned char)s.s[i]) || ++digits > 15) return false;
        v = v * 10 + (uint64_t)(s.s[i] - '0');
        run++;
    }
    *value = v;
    return true;
}

static const char *const unit_words[] = {"zero",    "one",     "two",       "three",    "four",
                                         "five",    "six",     "seven",     "eight",    "nine",
                                         "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                                         "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
static const char *const tens_words[] = {"twenty", "thirty",  "forty",  "fifty",
                                         "sixty",  "seventy", "eighty", "ninety"};
static const char *const scale_words[] = {"thousand", "million", "billion"};

static int word_index(span_t w, const char *const *words, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (strlen(words[i]) == w.n && !memcmp(words[i], w.s, w.n)) return (int)i;
    return -1;
}
static int unit_of(span_t w) { return word_index(w, unit_words, 20); }
static int tens_of(span_t w) {
    int i = word_index(w, tens_words, 8);
    return i < 0 ? -1 : 20 + 10 * i;
}
static int scale_of(span_t w) {
    int i = word_index(w, scale_words, 3);
    return i < 0 ? -1 : i + 1;
}
static bool word_is(span_t w, const char *s) { return strlen(s) == w.n && !memcmp(s, w.s, w.n); }

/* below100 at ws[i]; *next receives the rest. */
static bool below100(const span_t *ws, size_t n, size_t i, uint64_t *v, size_t *next) {
    if (i >= n) return false;
    int u = unit_of(ws[i]);
    if (u >= 0) {
        *v = (uint64_t)u;
        *next = i + 1;
        return true;
    }
    int t = tens_of(ws[i]);
    if (t < 0) return false;
    int u2 = i + 1 < n ? unit_of(ws[i + 1]) : -1;
    if (u2 >= 1 && u2 <= 9) {
        *v = (uint64_t)t + (uint64_t)u2;
        *next = i + 2;
    } else {
        *v = (uint64_t)t;
        *next = i + 1;
    }
    return true;
}

static bool below1000(const span_t *ws, size_t n, size_t i, uint64_t *v, size_t *next) {
    if (i + 1 < n) {
        int u = unit_of(ws[i]);
        if (u >= 1 && u <= 9 && word_is(ws[i + 1], "hundred")) {
            size_t rest = i + 2, after = rest;
            bool had_and = rest < n && word_is(ws[rest], "and");
            if (had_and) after++;
            uint64_t tail;
            size_t tail_next;
            if (below100(ws, n, after, &tail, &tail_next) && tail >= 1) {
                *v = 100u * (uint64_t)u + tail;
                *next = tail_next;
                return true;
            }
            if (had_and) return false;
            *v = 100u * (uint64_t)u;
            *next = rest;
            return true;
        }
    }
    return below100(ws, n, i, v, next);
}

static bool words_value(const span_t *ws, size_t n, uint64_t *value) {
    if (n == 1 && word_is(ws[0], "zero")) {
        *value = 0;
        return true;
    }
    uint64_t total = 0;
    int limit = 4;
    size_t i = 0;
    for (;;) {
        uint64_t v;
        size_t j;
        if (!below1000(ws, n, i, &v, &j) || !v) return false;
        if (j == n) {
            *value = total + v;
            return true;
        }
        int k = scale_of(ws[j]);
        if (k < 0 || k >= limit) return false;
        uint64_t scale = k == 1 ? 1000u : k == 2 ? 1000000u : 1000000000u;
        total += v * scale;
        if (j + 1 == n) {
            *value = total;
            return true;
        }
        limit = k;
        i = j + 1;
    }
}

bool tny_norm_number_value(const char *s, size_t n, uint64_t *value) {
    span_t core = trim_by((span_t){s, n}, is_edge_or_space);
    if (numeral_value(core, value)) return true;
    if (core.n > 256) return false; /* far longer than any cardinal below 10^12 */
    char folded[256];
    span_t ws[64];
    size_t count = 0;
    for (size_t i = 0; i < core.n; i++)
        folded[i] = core.s[i] == '-' ? ' ' : (char)lower((unsigned char)core.s[i]);
    size_t at = 0;
    span_t tok;
    while (next_token((span_t){folded, core.n}, &at, &tok)) {
        if (count == sizeof ws / sizeof ws[0]) return false;
        ws[count++] = tok;
    }
    return words_value(ws, count, value);
}

static bool number_ok(span_t span, span_t repl) {
    uint64_t a, b;
    return numeral_value(trim_by(repl, is_edge_or_space), &a) &&
           tny_norm_number_value(span.s, span.n, &b) && a == b;
}

/* ---- dictionary corrections (Lean: wordMatches, spanOk, dictOk) ---- */

static bool word_matches(const tny_dictionary_entry *e, span_t repl) {
    span_t a = trim_by(repl, is_edge_or_space);
    span_t b = trim_by((span_t){e->word, strlen(e->word)}, is_edge_or_space);
    return e->exact ? bytes_equal(a, b) : folded_equal(a, b);
}

static int span_ok(const tny_dictionary_entry *e, span_t span) {
    int meaning = sem_nonempty(span);
    if (meaning <= 0) return meaning;
    if (token_count(span) == 1) return 1;
    for (size_t i = 0; i < e->n_aliases; i++) {
        int eq = sem_equal((span_t){e->aliases[i], strlen(e->aliases[i])}, span);
        if (eq) return eq;
    }
    return 0;
}

/* 1 admissible, 0 not, -1 out of memory. */
static int admissible(const tny_dictionary *d, const tny_norm_correction *c) {
    span_t span = {c->span, c->span_len}, repl = {c->replacement, c->replacement_len};
    switch (c->reason) {
    case TNY_NORM_REASON_CASE: return folded_equal(span, repl);
    case TNY_NORM_REASON_PUNCTUATION: return sem_equal(span, repl);
    case TNY_NORM_REASON_NUMBER: return number_ok(span, repl);
    case TNY_NORM_REASON_DICTIONARY: break;
    }
    for (size_t i = 0; d && i < d->n; i++) {
        if (!word_matches(&d->entries[i], repl)) continue;
        int ok = span_ok(&d->entries[i], span);
        if (ok) return ok;
    }
    return 0;
}

bool tny_norm_admissible(const tny_dictionary *d, const tny_norm_correction *c) {
    return admissible(d, c) == 1;
}

/* ---- reconstruction (Lean: Dictation.Apply.applyAll) ---- */

static const char *find(const char *hay, size_t hn, const char *needle, size_t nn) {
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == needle[0] && !memcmp(hay + i, needle, nn)) return hay + i;
    return NULL;
}

/* Rebuild raw with the corrections and compare with text on the fly. */
static bool reconstructs(const char *raw, size_t raw_len, const tny_norm_proposal *p) {
    size_t cursor = 0, out = 0;
    for (size_t i = 0; i < p->n; i++) {
        const tny_norm_correction *c = &p->corrections[i];
        const char *at = find(raw + cursor, raw_len - cursor, c->span, c->span_len);
        if (!at) return false;
        size_t gap = (size_t)(at - (raw + cursor));
        if (gap > p->text_len - out || memcmp(p->text + out, raw + cursor, gap) != 0) return false;
        out += gap;
        if (c->replacement_len > p->text_len - out ||
            memcmp(p->text + out, c->replacement, c->replacement_len) != 0)
            return false;
        out += c->replacement_len;
        cursor += gap + c->span_len;
    }
    size_t rest = raw_len - cursor;
    return rest == p->text_len - out && !memcmp(p->text + out, raw + cursor, rest);
}

/* ---- edit bound (Lean: lev, editBound) ---- */

/* Banded Levenshtein: only cells with |i - j| <= k can hold a value <= k. */
static bool lev_within(const sem_tokens *a, const sem_tokens *b, size_t k) {
    size_t n = a->n, m = b->n;
    if (n > m + k || m > n + k) return false;
    size_t inf = k + 1;
    /* Zeroed rows: every read of the band is written first, and calloc makes
     * that checkable (a missed initialisation cannot pass by chance). */
    size_t *prev = calloc(m + 1, sizeof *prev), *cur = calloc(m + 1, sizeof *cur);
    bool ok = prev && cur;
    if (ok) {
        for (size_t j = 0; j <= m && j <= k + 1; j++) prev[j] = j <= k ? j : inf;
        for (size_t i = 1; i <= n; i++) {
            size_t lo = i > k ? i - k : 0, hi = i + k < m ? i + k : m;
            if (lo) cur[lo - 1] = inf;
            if (!lo) cur[0] = i;
            for (size_t j = lo ? lo : 1; j <= hi; j++) {
                size_t v = prev[j - 1] + (bytes_equal(sem_at(a, i - 1), sem_at(b, j - 1)) ? 0 : 1);
                if (prev[j] + 1 < v) v = prev[j] + 1;
                if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
                cur[j] = v < inf ? v : inf;
            }
            if (hi < m) cur[hi + 1] = inf;
            size_t *swap = prev;
            prev = cur;
            cur = swap;
        }
        ok = prev[m] <= k;
    }
    free(prev);
    free(cur);
    return ok;
}

int tny_norm_within_edit_bound(const char *a, size_t an, const char *b, size_t bn) {
    sem_tokens x, y;
    sem_build((span_t){a, an}, &x);
    sem_build((span_t){b, bn}, &y);
    int rc = x.oom || y.oom ? -1 : lev_within(&x, &y, 3 + x.n / 4);
    sem_free(&x);
    sem_free(&y);
    return rc;
}

/* ---- verify (Lean: Dictation.Verify.verify) ---- */

tny_norm_verdict tny_norm_verify(const tny_dictionary *d, const char *raw, size_t raw_len,
                                 const tny_norm_proposal *p) {
    if (!tny_dictation_text_valid(p->text, p->text_len)) return TNY_NORM_VERDICT_INVALID_TEXT;
    if (p->n > TNY_NORMALIZE_CORRECTIONS_MAX) return TNY_NORM_VERDICT_TOO_MANY;
    for (size_t i = 0; i < p->n; i++) {
        if (!p->corrections[i].span_len) return TNY_NORM_VERDICT_EMPTY_SPAN;
        int ok = admissible(d, &p->corrections[i]);
        if (ok < 0) return TNY_NORM_VERDICT_OOM;
        if (!ok) return TNY_NORM_VERDICT_INADMISSIBLE;
    }
    if (!reconstructs(raw, raw_len, p)) return TNY_NORM_VERDICT_UNLISTED;
    int within = tny_norm_within_edit_bound(raw, raw_len, p->text, p->text_len);
    if (within < 0) return TNY_NORM_VERDICT_OOM;
    return within ? TNY_NORM_VERDICT_OK : TNY_NORM_VERDICT_EDIT_BOUND;
}

const char *tny_norm_verdict_name(tny_norm_verdict v) {
    static const char *const names[] = {"ok",         "invalid_text", "too_many",
                                        "empty_span", "inadmissible", "unlisted",
                                        "edit_bound", "out_of_memory"};
    return (unsigned)v < sizeof names / sizeof names[0] ? names[v] : "unknown";
}

const char *tny_norm_reason_name(tny_norm_reason r) {
    static const char *const names[] = {"dictionary", "case", "punctuation", "number"};
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "unknown";
}

/* ---- strict proposal parse ---- */

void tny_norm_proposal_free(tny_norm_proposal *p) {
    if (!p) return;
    if (p->text) secure_zero(p->text, p->text_len);
    free(p->text);
    for (size_t i = 0; i < p->n; i++) {
        free(p->corrections[i].span);
        free(p->corrections[i].replacement);
    }
    free(p->corrections);
    memset(p, 0, sizeof *p);
}

static bool copy_string(yyjson_val *v, char **out, size_t *len) {
    const char *s = yyjson_get_str(v);
    if (!s) return false;
    size_t n = yyjson_get_len(v);
    if (memchr(s, 0, n)) return false;
    *out = xstrndup(s, n);
    *len = n;
    return *out != NULL;
}

bool tny_norm_proposal_parse(const char *json, size_t len, tny_norm_proposal *out) {
    memset(out, 0, sizeof *out);
    yyjson_doc *doc = jparse(json, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *corrections = jget(root, "corrections");
    bool ok = yyjson_is_obj(root) && yyjson_obj_size(root) == 2 && yyjson_is_arr(corrections) &&
              copy_string(jget(root, "text"), &out->text, &out->text_len);
    size_t n = ok ? yyjson_arr_size(corrections) : 0;
    if (ok && n) {
        out->corrections = calloc(n, sizeof *out->corrections);
        ok = out->corrections != NULL;
    }
    size_t i, max;
    yyjson_val *item;
    yyjson_arr_foreach(ok ? corrections : NULL, i, max, item) {
        tny_norm_correction *c = &out->corrections[out->n];
        const char *reason = jget_str(item, "reason");
        int r = -1;
        for (int k = 0; reason && k < 4; k++)
            if (!strcmp(reason, tny_norm_reason_name((tny_norm_reason)k))) r = k;
        if (!yyjson_is_obj(item) || yyjson_obj_size(item) != 3 || r < 0 ||
            !copy_string(jget(item, "span"), &c->span, &c->span_len) ||
            !copy_string(jget(item, "replacement"), &c->replacement, &c->replacement_len)) {
            free(c->span);
            c->span = NULL;
            ok = false;
            break;
        }
        c->reason = (tny_norm_reason)r;
        out->n++;
    }
    yyjson_doc_free(doc);
    if (!ok) tny_norm_proposal_free(out);
    return ok;
}

/* ---- lifecycle (Lean: Dictation.Lifecycle.step) ---- */

tny_norm_state tny_norm_init(bool enabled) {
    return (tny_norm_state){TNY_NORM_TRANSCRIBING, enabled, false, 0, TNY_NORM_PENDING};
}

tny_norm_state tny_norm_step(tny_norm_state s, tny_norm_event e) {
    switch (e) {
    case TNY_NORM_EV_STT_INVALID:
    case TNY_NORM_EV_STT_VALID:
        if (s.phase != TNY_NORM_TRANSCRIBING) return s;
        if (e == TNY_NORM_EV_STT_INVALID) {
            s.phase = TNY_NORM_DONE;
            s.outcome = TNY_NORM_FAILED;
        } else if (s.enabled) {
            s.phase = TNY_NORM_NORMALIZING;
            s.effort = true;
            s.requests = 1;
        } else {
            s.phase = TNY_NORM_DONE;
            s.outcome = TNY_NORM_RAW;
        }
        return s;
    case TNY_NORM_EV_STT_FAILED:
        if (s.phase == TNY_NORM_TRANSCRIBING) {
            s.phase = TNY_NORM_DONE;
            s.outcome = TNY_NORM_FAILED;
        }
        return s;
    case TNY_NORM_EV_CANCEL:
        if (s.phase == TNY_NORM_TRANSCRIBING) s.outcome = TNY_NORM_CANCELLED;
        else if (s.phase == TNY_NORM_NORMALIZING) s.outcome = TNY_NORM_RAW;
        else return s;
        s.phase = TNY_NORM_DONE;
        return s;
    case TNY_NORM_EV_EFFORT_REJECTED:
        if (s.phase != TNY_NORM_NORMALIZING) return s;
        if (s.effort && s.requests < 2) {
            s.effort = false;
            s.requests++;
        } else {
            s.phase = TNY_NORM_DONE;
            s.outcome = TNY_NORM_RAW;
        }
        return s;
    case TNY_NORM_EV_FAILED:
    case TNY_NORM_EV_REJECTED:
    case TNY_NORM_EV_ACCEPTED:
        if (s.phase != TNY_NORM_NORMALIZING) return s;
        s.phase = TNY_NORM_DONE;
        s.outcome = e == TNY_NORM_EV_ACCEPTED ? TNY_NORM_NORMALIZED : TNY_NORM_RAW;
        return s;
    }
    return s;
}
