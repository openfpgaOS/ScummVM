/*
 * of_audio.h -- Audio subsystem API for openfpgaOS
 *
 * 48 kHz stereo PCM path.  Apps write interleaved stereo s16 pairs; the
 * OS software mixer composes them with SF2 voices and mixer SFX into a
 * single stream and pushes it to the audio FIFO.
 *
 * MIDI playback is layered on top of the same software mixer via
 * of_midi.h / of_smp_voice.h.
 */

#ifndef OF_AUDIO_H
#define OF_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_AUDIO_RATE   48000
#define OF_AUDIO_FIFO   1024   /* stereo pairs; matches audio_output.v dcfifo depth */

#ifndef OF_PC

#include "of_services.h"

static inline void of_audio_init(void) {
    OF_SVC->audio_init();
}

static inline int of_audio_write(const int16_t *samples, int count) {
    return OF_SVC->audio_write(samples, count);
}

static inline int of_audio_free(void) {
    return OF_SVC->audio_get_free();
}

/* Streaming audio: double-buffered gapless playback for music/voice. */
static inline int of_audio_stream_open(int sample_rate) {
    return OF_SVC->audio_stream_open(sample_rate);
}

static inline int of_audio_stream_write(const int16_t *samples, int count) {
    return OF_SVC->audio_stream_write(samples, count);
}

static inline int of_audio_stream_ready(void) {
    return OF_SVC->audio_stream_ready();
}

static inline void of_audio_stream_close(void) {
    OF_SVC->audio_stream_close();
}

#else /* OF_PC */

void of_audio_init(void);
int  of_audio_write(const int16_t *samples, int count);
int  of_audio_free(void);
static inline int of_audio_stream_open(int sample_rate) { (void)sample_rate; return -1; }
static inline int of_audio_stream_write(const int16_t *samples, int count) { (void)samples; (void)count; return 0; }
static inline int of_audio_stream_ready(void) { return 1; }
static inline void of_audio_stream_close(void) {}
#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_AUDIO_H */
