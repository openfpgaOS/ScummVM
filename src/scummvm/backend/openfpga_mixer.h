/*
 * openfpga_mixer.h -- ScummVM MixerManager bridging Audio::MixerImpl to
 *                     the SDK's low-level stereo PCM path (of_audio_write).
 *
 * Sample format: interleaved stereo signed 16-bit @ 48 kHz (OF_AUDIO_RATE,
 * the SDK's fixed stream rate).  We mix our own stereo bus and push it with
 * of_audio_write(), using of_audio_free() for exact backpressure against the
 * OS-owned hardware ring.  We deliberately do NOT use of_audio_stream_*
 * (mono / resampled) -- see openfpga_mixer.cpp for the rationale.
 *
 * Everything runs on the main thread.  update() (producer) mixes blocks and
 * pushes them while the HW ring has room; drain() (cheap) advances the SDK
 * software-mixer voices (MIDI synth, CD-audio).  Both are driven from the
 * OSystem pump sites (delayMillis / pollEvent / updateScreen) -- never from
 * an ISR (of_audio_write is not IRQ-safe; see openfpga_midi.cpp).
 */

#ifndef OPENFPGA_MIXER_H
#define OPENFPGA_MIXER_H

#include "backends/mixer/mixer.h"

extern "C" {
#include <of.h>
#include <stdint.h>
}

class OpenFPGAMixerManager : public MixerManager {
public:
    OpenFPGAMixerManager();
    ~OpenFPGAMixerManager() override;

    void init() override;

    /* PRODUCER (main thread): mixes one or more stereo blocks into _buffer
     * and pushes them to the HW ring via of_audio_write() while of_audio_free()
     * reports room.  Expensive (mixCallback decodes/mixes); bursty by design.
     * Called from pollEvent / updateScreen and the delayMillis sleep loop. */
    void update();

    /* DRAINER (main thread): advances the SDK software-mixer voices via
     * of_mixer_pump().  Cheap; called at ~1 ms granularity so MIDI / CD-audio
     * voices stay fed between the producer's mix bursts. */
    void drain();

    void suspendAudio() override;
    int  resumeAudio() override;

private:
    bool      _initDone;       /* of_audio / of_mixer initialized */
    uint32_t  _outputRate;     /* OF_AUDIO_RATE (48 kHz) */
    uint32_t  _framesPerBlock; /* stereo frames mixed per of_audio_write */
    int16_t  *_buffer;         /* _framesPerBlock * 2 (stereo) int16 scratch */
    int       _ringCapacity;   /* HW ring depth in stereo pairs, measured at
                                * init (0 until measured) -- used to cap the
                                * buffered cushion so SFX/speech stay low-latency */
};

#endif /* OPENFPGA_MIXER_H */
