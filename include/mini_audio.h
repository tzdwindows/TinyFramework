#ifndef MINI_AUDIO_H
#define MINI_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mini_audio_init(int sample_rate, int channels);
void mini_audio_shutdown(void);
void mini_audio_queue_pcm(const float *samples, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* MINI_AUDIO_H */
