/*
 * openfpga_mixer.cpp -- ScummVM MixerManager bridging Audio::MixerImpl
 *                       to the SDK's stereo PCM FIFO.
 *
 * The SDK exposes two audio paths:
 *
 *   of_audio_stream_*   -- MONO, resampled; designed for one music/voice
 *                          stream.  Cannot carry our stereo mix.
 *   of_audio_init /
 *   of_audio_write /    -- STEREO interleaved s16 at 48 kHz, with
 *   of_audio_free          of_audio_free() returning the exact number
 *                          of free stereo pairs in the hardware FIFO.
 *
 * We use the low-level stereo API because (a) our mix is stereo and
 * (b) of_audio_free gives us precise backpressure, letting us match
 * production to the DAC's consumption rate exactly.
 *
 * Architecture:
 *
 *   producer (update)  ->  ring buffer (~170 ms)  ->  drainer (drain)
 *   - main thread          - in SDRAM                 - main thread
 *   - variable cadence     - 8192 frames stereo       - frequent (1 kHz)
 *   - expensive            - lock-free SPSC           - cheap memcpy + write
 *
 * IRQ-side drain is NOT used: of_audio_write is not re-entrant with
 * respect to the main-thread allocator and corrupts the heap when
 * invoked from the 1 kHz timer ISR.  Instead, drain() is wired into
 * the OSystem delayMillis sleep loop at 1 ms granularity (see
 * openfpga_osystem.cpp::delayMillis), and is also called from
 * pollEvent and updateScreen so blits/event handling don't starve it.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_mixer.h"

#include "audio/mixer_intern.h"

extern "C" {
#include <stdlib.h>
#include <string.h>
#include <of_mixer.h>

void midi_tick_irq(void);   /* in openfpga_midi.cpp */
}

OpenFPGAMixerManager::OpenFPGAMixerManager()
    : MixerManager(),
      _streamOpen(false),
      _outputRate(OF_AUDIO_RATE),  /* 48000 -- SDK's fixed stream rate */
      _framesPerBlock(256),
      _buffer(nullptr),
      _ring(nullptr),
      _ringHead(0),
      _ringTail(0) {}

OpenFPGAMixerManager::~OpenFPGAMixerManager() {
    /* of_audio has no explicit close; just stop pushing. */
    free(_buffer);
    free(_ring);
}

void OpenFPGAMixerManager::init() {
    _mixer = new Audio::MixerImpl(_outputRate, true, _framesPerBlock);
    _mixer->setReady(true);

    /* Scratch buffer used by mixCallback (one block of stereo PCM). */
    _buffer = (int16_t *)calloc(_framesPerBlock * 2, sizeof(int16_t));

    /* Ring buffer: pre-mixed audio waiting to be pushed to FIFO. */
    _ring = (int16_t *)calloc(RING_FRAMES * 2, sizeof(int16_t));

    /* Match the Duke3D SDK audio setup.  ScummVM's Audio::MixerImpl keeps
     * music/SFX/speech volume policy internally and then feeds one stereo
     * PCM stream through of_audio_write().  The SDK groups below apply to
     * direct SDK mixer voices, currently the openfpga MIDI synth; those
     * voices are allocated in OF_MIXER_GROUP_MUSIC by of_smp_voice. */
    of_mixer_init(48, OF_MIXER_OUTPUT_RATE);
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_VOICE, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_AUX, 255);

    of_audio_init();
    /* of_audio_stream_open is intentionally NOT called.  The SDL2
     * shim calls it only to retarget mixer voice 31 to a non-48 kHz
     * rate; at 48 kHz the SDK's default config already plays
     * of_audio_write samples 1:1.  Calling it disturbs the FIFO in
     * a way that produces resonance / vibrato on CD-audio PCM. */
    _streamOpen = true;

    /* IRQ-side audio drain (commented out): of_audio_write turned out
     * to be unsafe from the timer ISR after all -- IRQ-driven drain
     * produced a vibrating modulation in playback, worse than the
     * occasional underrun we get with main-thread drain.  The drain
     * is now called only from delayMillis / pollEvent / updateScreen
     * (see openfpga_osystem.cpp). */
    /* of_timer_set_callback(midi_tick_irq, 1000); */

    /* Publish ourselves to the IRQ-side drain shim (no-op currently
     * because the ISR call site is disabled; kept for future use if
     * the SDK ever provides an IRQ-safe write path). */
    extern OpenFPGAMixerManager *g_pumpMixerMgrForIrq;
    g_pumpMixerMgrForIrq = this;
}

void OpenFPGAMixerManager::update() {
    if (_audioSuspended || !_streamOpen || !_mixer || !_buffer) return;

    /* Bare-bones SDL2 shim pattern: query free space, mix one block,
     * push.  No cache flush -- the shim doesn't do one, suggesting
     * of_audio_write does an internal CPU memcpy (no DMA from our
     * buffer) so coherency isn't our concern. */
    while (of_audio_free() >= (int)_framesPerBlock) {
        _mixer->mixCallback((uint8 *)_buffer, _framesPerBlock * 4);
        int wrote = of_audio_write(_buffer, (int)_framesPerBlock);
        if (wrote <= 0) break;
    }
}

void OpenFPGAMixerManager::drain() {
    if (!_streamOpen)
        return;
    of_mixer_pump();
}

/* Legacy alias kept so the existing IRQ shim symbol still resolves.
 * Not called from IRQ (see file header). */
void OpenFPGAMixerManager::drainFromIRQ() { drain(); }

extern "C" void openfpga_mixer_drain_irq(void) {
    extern OpenFPGAMixerManager *g_pumpMixerMgrForIrq;
    if (g_pumpMixerMgrForIrq)
        g_pumpMixerMgrForIrq->drain();
}

OpenFPGAMixerManager *g_pumpMixerMgrForIrq = nullptr;

void OpenFPGAMixerManager::suspendAudio() {
    _audioSuspended = true;
}

int OpenFPGAMixerManager::resumeAudio() {
    if (!_audioSuspended) return -2;
    _audioSuspended = false;
    return 0;
}
