/* Transcript normalization (ADR 0175): user dictionary, deterministic
 * verifier, lifecycle and the structured rewrite request. Private to core
 * dictation. The pure parts are the C side of the Lean specification in
 * tests/formal/dictation and are exposed for its golden-table replay. */
#ifndef TNY_DICTATION_NORMALIZE_H
#define TNY_DICTATION_NORMALIZE_H
#include "core/dictation.h"

#define TNY_DICTIONARY_FILE_MAX       (64u * 1024u)
#define TNY_DICTIONARY_ENTRIES_MAX    256u
#define TNY_DICTIONARY_WORD_MAX       64u
#define TNY_DICTIONARY_CONTEXT_MAX    256u
#define TNY_DICTIONARY_ALIASES_MAX    8u
#define TNY_NORMALIZE_CORRECTIONS_MAX 64u
#define TNY_NORMALIZE_TIMEOUT_DEFAULT 20
#define TNY_NORMALIZE_TIMEOUT_MAX     120
#define TNY_NORMALIZE_MODEL_CODEX     "gpt-6-luna"
#define TNY_NORMALIZE_MODEL_XAI       "grok-4.7"

/* ---- dictionary (Lean: Dictation.Dictionary) ---- */

typedef struct {
    char *word;
    char *context; /* possibly empty */
    bool exact;    /* "case": "exact" */
    char **aliases;
    size_t n_aliases;
} tny_dictionary_entry;

typedef struct {
    tny_dictionary_entry *entries;
    size_t n;
} tny_dictionary;

/* Validate one dictionary file. On failure *out is empty and err explains. */
bool tny_dictionary_parse(const char *json, size_t len, tny_dictionary *out, char *err,
                          size_t errlen);
/* Project entries, then user entries whose word the project lacks. Consumes
 * both inputs (they are left empty) even when allocation fails. */
bool tny_dictionary_merge(tny_dictionary *user, tny_dictionary *project, tny_dictionary *out);
/* ~/.tny/dictionary.json merged under <cwd>/.tny/dictionary.json. A missing
 * file is empty; an unreadable, oversized or invalid one fails. */
bool tny_dictionary_load(const char *cwd, tny_dictionary *out, char *err, size_t errlen);
void tny_dictionary_free(tny_dictionary *);

/* ---- proposal and verifier (Lean: Dictation.Verify) ---- */

typedef enum {
    TNY_NORM_REASON_DICTIONARY,
    TNY_NORM_REASON_CASE,
    TNY_NORM_REASON_PUNCTUATION,
    TNY_NORM_REASON_NUMBER
} tny_norm_reason;

typedef struct {
    char *span, *replacement;
    size_t span_len, replacement_len;
    tny_norm_reason reason;
} tny_norm_correction;

typedef struct {
    char *text;
    size_t text_len;
    tny_norm_correction *corrections;
    size_t n;
} tny_norm_proposal;

/* Strict: exactly {"text": string, "corrections": [{"span", "replacement",
 * "reason"}]} with no other keys and no NUL bytes. */
bool tny_norm_proposal_parse(const char *json, size_t len, tny_norm_proposal *out);
void tny_norm_proposal_free(tny_norm_proposal *);
const char *tny_norm_reason_name(tny_norm_reason);

typedef enum {
    TNY_NORM_VERDICT_OK,
    TNY_NORM_VERDICT_INVALID_TEXT,
    TNY_NORM_VERDICT_TOO_MANY,
    TNY_NORM_VERDICT_EMPTY_SPAN,
    TNY_NORM_VERDICT_INADMISSIBLE,
    TNY_NORM_VERDICT_UNLISTED,
    TNY_NORM_VERDICT_EDIT_BOUND,
    TNY_NORM_VERDICT_OOM
} tny_norm_verdict;

const char *tny_norm_verdict_name(tny_norm_verdict);
tny_norm_verdict tny_norm_verify(const tny_dictionary *, const char *raw, size_t raw_len,
                                 const tny_norm_proposal *);
/* Exposed pieces of the verifier, for focused unit and mutation tests. */
bool tny_norm_admissible(const tny_dictionary *, const tny_norm_correction *);
bool tny_norm_number_value(const char *s, size_t n, uint64_t *value);
/* Semantic-token Levenshtein distance of a and b is at most 3 + n / 4, where n
 * counts a's semantic tokens (Lean: editBound). */
int tny_norm_within_edit_bound(const char *a, size_t an, const char *b, size_t bn);

/* ---- lifecycle (Lean: Dictation.Lifecycle.step) ---- */

typedef enum { TNY_NORM_TRANSCRIBING, TNY_NORM_NORMALIZING, TNY_NORM_DONE } tny_norm_phase;
typedef enum {
    TNY_NORM_PENDING,
    TNY_NORM_FAILED,
    TNY_NORM_CANCELLED,
    TNY_NORM_RAW,
    TNY_NORM_NORMALIZED
} tny_norm_outcome;
typedef enum {
    TNY_NORM_EV_STT_INVALID,
    TNY_NORM_EV_STT_VALID,
    TNY_NORM_EV_STT_FAILED,
    TNY_NORM_EV_CANCEL,
    TNY_NORM_EV_EFFORT_REJECTED,
    TNY_NORM_EV_FAILED,
    TNY_NORM_EV_REJECTED,
    TNY_NORM_EV_ACCEPTED
} tny_norm_event;

typedef struct {
    tny_norm_phase phase;
    bool enabled;
    bool effort; /* the in-flight request may be retried without effort */
    unsigned requests;
    tny_norm_outcome outcome;
} tny_norm_state;

tny_norm_state tny_norm_init(bool enabled);
tny_norm_state tny_norm_step(tny_norm_state, tny_norm_event);

/* ---- configuration and request ---- */

typedef struct {
    bool enabled;
    bool valid; /* false: enabled but misconfigured (fails open) */
    char model[129];
    char effort[33]; /* wire value; empty omits the field */
    bool fast;       /* codex only: service_tier "priority" */
    int timeout_seconds;
} tny_norm_config;

/* mode: TNY_DICTATION_NORMALIZE_{DEFAULT,ON,OFF}. Precedence: explicit mode,
 * TNY_DICTATION_NORMALIZE, settings dictation.normalize, off. The model comes
 * from TNY_DICTATION_NORMALIZE_MODEL, settings, then the adapter default. */
void tny_norm_config_resolve(const tny_ctx *, const char *adapter, int mode, tny_norm_config *,
                             char *err, size_t errlen);

/* Endpoint chosen by the STT adapter from the credential it already uses. */
typedef struct {
    char *url;
    char *headers[8]; /* NULL-terminated owned "Name: value" lines */
    bool chat;        /* Chat Completions wire (else Responses) */
    bool schema;      /* send a JSON schema (text.format / response_format) */
    bool tier;        /* service_tier is supported */
} tny_norm_target;

void tny_norm_target_free(tny_norm_target *);

typedef struct tny_norm_job tny_norm_job;

tny_norm_job *tny_norm_job_start(const tny_norm_target *, const tny_norm_config *, bool effort,
                                 const tny_dictionary *, const char *raw, int64_t deadline,
                                 char *reason, size_t len);
int tny_norm_job_fd(const tny_norm_job *);
/* -1 pending, 0 output text in *out, 1 failure (reason set), 3 the effort
 * field was rejected (HTTP 400/422 while it was sent). */
int tny_norm_job_step(tny_norm_job *, buf_t *out, char *reason, size_t len);
void tny_norm_job_free(tny_norm_job *);
#endif
