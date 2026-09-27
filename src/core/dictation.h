/* Provider-independent microphone/file-to-text service (ADR 0079).
 * No display, agent turns, session writes, or chat-provider selection. */
#ifndef TNY_DICTATION_H
#define TNY_DICTATION_H
#include "core/config.h"

#define TNY_DICTATION_AUDIO_MAX   (25u * 1024u * 1024u)
#define TNY_DICTATION_TEXT_MAX    (64u * 1024u)
#define TNY_DICTATION_SECONDS_MAX 300

typedef enum {
    TNY_DICTATION_RECORDING,
    TNY_DICTATION_TRANSCRIBING,
    TNY_DICTATION_NORMALIZING, /* optional rewrite, ADR 0175 */
    TNY_DICTATION_DONE
} tny_dictation_state;

/* Transcript normalization: settings/environment default, or forced. */
enum { TNY_DICTATION_NORMALIZE_DEFAULT, TNY_DICTATION_NORMALIZE_ON, TNY_DICTATION_NORMALIZE_OFF };

typedef struct {
    const char *provider;   /* NULL: TNY_STT_PROVIDER, then codex */
    const char *input_file; /* NULL: microphone; otherwise a PCM16 WAV file */
    const char *device;     /* NULL: TNY_AUDIO_DEVICE, then default input */
    int seconds;            /* 0: stop explicitly; always capped at 300 seconds */
    int normalize;          /* TNY_DICTATION_NORMALIZE_* */
    bool (*cancelled)(void *);
    void *userdata;
} tny_dictation_request;

typedef struct tny_dictation tny_dictation;

/* Local capability check only; never opens a microphone, refreshes or connects. */
bool tny_dictation_available(const tny_ctx *, const char *provider, bool microphone, char *err,
                             size_t errlen);
/* ctx must outlive the operation. Only the selected adapter may read credentials. */
tny_dictation *tny_dictation_start(const tny_ctx *, const tny_dictation_request *, char *, size_t);
tny_dictation_state tny_dictation_get_state(const tny_dictation *);
const char *tny_dictation_provider_name(const tny_dictation *);
int tny_dictation_fd(const tny_dictation *); /* -1 when no fd is ready to poll */
/* Drive from the existing event loop at least every 50 ms while active. */
void tny_dictation_step(tny_dictation *);
void tny_dictation_finish(tny_dictation *); /* stop recording, then transcribe */
/* Discard audio/text and stop I/O. While NORMALIZING it stops only the rewrite
 * and completes successfully with the raw transcript. */
void tny_dictation_cancel(tny_dictation *);
/* After DONE: 0 success, 1 local/protocol error, 2 HTTP rejection, 130 cancelled. */
int tny_dictation_result(const tny_dictation *, const char **text, const char **error);

/* After a successful result, when normalization was enabled for it. Borrowed
 * strings live until tny_dictation_free. */
typedef struct {
    bool normalized; /* the delivered text is the verified rewrite */
    const char *raw; /* the transcript as returned by STT */
    const char *model;
    const char *effort;           /* sent on the last request; NULL when omitted */
    const char *service_tier;     /* NULL unless sent */
    const char *skipped_reason;   /* bounded reason when not normalized, else NULL */
    const char *detail;           /* configuration/dictionary diagnostic, or "" */
    const char *corrections_json; /* accepted corrections as a JSON array */
} tny_dictation_normalization;
bool tny_dictation_normalization_info(const tny_dictation *, tny_dictation_normalization *);
void tny_dictation_free(tny_dictation *);

/* Pure shared validation, exposed for bounded fixture tests. */
bool tny_dictation_wav_valid(const void *, size_t);
bool tny_dictation_text_valid(const void *, size_t);
#endif
