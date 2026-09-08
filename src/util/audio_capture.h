/* Host OS microphone seam: bounded PCM16 LE, mono, 24 kHz on a pipe.
 * No audio library, device access or process launch until start(). */
#ifndef TNY_AUDIO_CAPTURE_H
#define TNY_AUDIO_CAPTURE_H
#include "util/util.h"

#define AUDIO_CAPTURE_RATE 24000u
typedef struct audio_capture audio_capture;
bool audio_capture_available(void);
audio_capture *audio_capture_start(const char *device, char *err, size_t errlen);
int audio_capture_fd(const audio_capture *);
/* Append at most limit total bytes. -1 failed, 0 still recording, 1 stopped. */
int audio_capture_read(audio_capture *, buf_t *, size_t limit, char *, size_t);
void audio_capture_stop(audio_capture *);
void audio_capture_free(audio_capture *);
#endif
