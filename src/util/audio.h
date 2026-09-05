/* Host audio seam: optional external MP3 player, no decoder dependency. */
#ifndef TNY_AUDIO_H
#define TNY_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
bool audio_player_available(void);
int audio_play(const void *data, size_t len, bool (*cancelled)(void *), void *ud, char *err,
               size_t errlen);
#endif
