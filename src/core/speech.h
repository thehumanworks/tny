/* speech.h — provider-independent speech service; ADR 0070. */
#ifndef TNY_SPEECH_H
#define TNY_SPEECH_H
#include "core/config.h"

#define TNY_SPEECH_TEXT_MAX  (16u * 1024u)
#define TNY_SPEECH_AUDIO_MAX (16u * 1024u * 1024u)

typedef struct {
    const char *text;
    const char *provider;    /* NULL selects codex, independent of chat provider */
    const char *voice;       /* NULL selects cove */
    const char *output_file; /* NULL plays; non-NULL exports without playing */
    bool (*cancelled)(void *ud);
    void *userdata;
} tny_speech_request;

/* Local capability check only: no network or token refresh. */
bool tny_speech_available(const tny_ctx *ctx, const char *provider, bool playback, char *err,
                          size_t errlen);
/* 0 success, 1 configuration/I/O failure, 2 provider rejection, 130 interrupted. */
int tny_speech_run(const tny_ctx *ctx, const tny_speech_request *request, char *err, size_t errlen);
#endif
