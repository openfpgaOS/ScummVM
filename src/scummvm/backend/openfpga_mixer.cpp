/*
 * openfpga_mixer.cpp -- ScummVM MixerManager bridging Audio::MixerImpl
 *                       to the SDK's stereo PCM path.
 *
 * The SDK exposes two audio paths:
 *
 *   of_audio_stream_*   -- MONO, resampled; designed for one music/voice
 *                          stream.  Cannot carry our stereo mix.
 *   of_audio_init /
 *   of_audio_write /    -- STEREO interleaved s16 at 48 kHz, with
 *   of_audio_free          of_audio_free() returning the exact number
 *                          of free stereo pairs in the hardware ring.
 *
 * We use the low-level stereo path because (a) our mix is stereo and
 * (b) of_audio_free gives us precise backpressure, letting us match
 * production to the DAC's consumption rate exactly.  of_audio_write copies
 * into the OS-owned uncached SDRAM ring, so no cache flush is needed on our
 * side (unlike the of_mixer voice DMA path used for CD-audio).
 *
 * The HW ring (~2.7 s on pocket) absorbs producer stalls and holds position
 * on underrun, so there is no app-side ring buffer here -- update() mixes
 * straight into a one-block scratch buffer and pushes it synchronously.
 *
 * Everything runs on the main thread.  An IRQ-side drain was tried and
 * abandoned: of_audio_write from the 1 kHz timer ISR corrupts the heap /
 * produces audible vibrato (see openfpga_midi.cpp).  drain() is wired into
 * the OSystem delayMillis sleep loop at 1 ms granularity and is also called
 * from pollEvent and updateScreen (see openfpga_osystem.cpp).
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_mixer.h"

#include "audio/mixer_intern.h"

extern "C" {
#include <stdlib.h>
#include <string.h>
#include <of_mixer.h>
#include <of_timer.h>
}

OpenFPGAMixerManager::OpenFPGAMixerManager()
    : MixerManager(),
      _initDone(false),
      _outputRate(OF_AUDIO_RATE),  /* 48000 -- SDK's fixed stream rate */
      _framesPerBlock(256),
      _buffer(nullptr),
      _ringCapacity(0) {}

/* Target buffered depth.  The OS hardware ring is ~2.7 s deep on pocket; if
 * we keep it full, a sound ScummVM mixes "now" lands at the FIFO tail and is
 * heard ~2.7 s later.  Hold only a small low-latency cushion instead so
 * SFX/speech are prompt.  48 stereo pairs = 1 ms @ 48 kHz.
 *
 * Resolution-dependent: the graphics manager raises the base cushion for
 * hi-res modes (SCI32 640x480 -- 4.8x the pixels of the 320x200 games) via
 * openfpga_mixer_set_base_cushion_ms(), because a single heavy frame's
 * compose+present can stretch the main thread past the 120 ms the low-res
 * games were tuned for.  Low-res games keep the original 120 ms; the deep
 * load cushion below is unaffected. */
static const int kTargetBufferedPairs  = 48 * 120;  /* ~120 ms (interactive) */
static int       g_targetBufferedPairs = kTargetBufferedPairs;

void openfpga_mixer_set_base_cushion_ms(uint32 ms) {
    if (ms < 60)  ms = 60;    /* below one video frame of slack is asking for it */
    if (ms > 500) ms = 500;   /* beyond this, new-sound latency is clearly audible */
    g_targetBufferedPairs = 48 * (int)ms;
}

/* Deep cushion used only while a load is in flight.  Heavy SCI room/screen
 * transitions spend stretches >120 ms in pure in-memory work (resource
 * decompress, room kAnimate setup, the transition effect) with NO file read
 * and NO updateScreen(), so neither audio-pump site fires and the 120 ms
 * cushion above underruns -- the "hiccup between loads".  openfpga_pump_during_
 * load() (FS read path) arms this deeper target for a short window, so the ring
 * is pre-filled enough to ride through the compute gap.  Latency is invisible
 * mid-transition (no interactive SFX), and the instant loading stops the target
 * drops back to 120 ms and the ring drains down to low latency on its own. */
static const int kLoadCushionPairs     = 48 * 800;  /* ~800 ms (during loads) */
static uint32     g_loadCushionUntilMs = 0;
static int        g_ringCapacityPairs  = 0;  /* full HW ring depth; set in init() */

/* Called from openfpga_pump_during_load() on every resource read: hold the deep
 * cushion until `untilMs` (of_time_ms() clock). */
void openfpga_mixer_extend_cushion(uint32 untilMs) {
    g_loadCushionUntilMs = untilMs;
}

/* ms of audio already mixed into the HW ring but NOT yet played.  This port
 * bursts the mixer ahead of the DAC, so getSoundElapsedTime (samples CONSUMED)
 * leads the audible voice by this much; SCI lip-sync (Portrait) subtracts it to
 * track what is actually heard. */
extern "C" uint32 openfpga_mixer_output_latency_ms(void) {
    if (g_ringCapacityPairs <= 0)
        return 0;
    int buffered = g_ringCapacityPairs - of_audio_free();
    if (buffered < 0)
        buffered = 0;
    return (uint32)(buffered * 1000 / OF_AUDIO_RATE);
}

OpenFPGAMixerManager::~OpenFPGAMixerManager() {
    /* of_audio has no explicit close; just stop pushing. */
    free(_buffer);
}

void OpenFPGAMixerManager::init() {
    _mixer = new Audio::MixerImpl(_outputRate, true, _framesPerBlock);
    _mixer->setReady(true);

    /* Scratch buffer used by mixCallback (one block of stereo PCM). */
    _buffer = (int16_t *)calloc(_framesPerBlock * 2, sizeof(int16_t));

    /* ScummVM's Audio::MixerImpl keeps music/SFX/speech volume policy
     * internally and feeds one stereo PCM stream through of_audio_write().
     * The SDK groups below apply to direct SDK mixer voices -- currently the
     * openfpga MIDI synth (OF_MIXER_GROUP_MUSIC) and CD-audio
     * (OF_MIXER_GROUP_AUX). */
    of_mixer_init(OF_MIXER_MAX_VOICES, OF_MIXER_OUTPUT_RATE);
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_VOICE, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_AUX, 255);

    /* Audio HW (of_audio_init) is brought up once in main().  Voice 31 is
     * configured 1:1 on the first of_audio_write at 48 kHz, so
     * of_audio_stream_open is intentionally NOT called -- at 48 kHz it only
     * retargets voice 31 for HW resampling and disturbs the FIFO, producing
     * resonance / vibrato on CD-audio PCM. */

    /* Measure the ring depth: of_audio_init ran in main() and voice 31 is
     * still inactive here, so of_audio_free() reports the full capacity. */
    _ringCapacity = of_audio_free();
    g_ringCapacityPairs = _ringCapacity;

    _initDone = true;
}

void OpenFPGAMixerManager::update() {
    if (_audioSuspended || !_initDone || !_mixer || !_buffer) return;

    /* Refill only up to a small cushion (kTargetBufferedPairs), never topping
     * the ~2.7 s ring -- otherwise newly-triggered SFX/speech queue behind
     * seconds of already-buffered audio.  of_audio_write copies into the
     * OS-owned uncached SDRAM ring, so no cache flush is required here.
     * While a load is in flight, fill the deeper kLoadCushionPairs instead so a
     * non-yielding compute gap can't drain the ring to silence (see above). */
    /* Never use the deep load cushion while position-paced PCM is playing.
     *
     * kSpeechSoundType: SCI lip-sync (Portrait, e.g. KQ6 talking heads)
     * paces on the audio position, and bursting a short voice clip fully
     * into the deep ring makes its handle finish early -> the portrait
     * stops after a few frames.
     *
     * kPlainSoundType: SCI32's Audio32 mixes ALL its digital audio (LSL7
     * speech, SFX, and Robot/talker audio) through one channel of this
     * type -- never kSpeech -- so the speech check alone left LSL7 fully
     * exposed.  Diagnosed on device 2026-07-16: LSL7's continuous
     * streaming reads pass the burst gate, so the cushion sat armed
     * through ordinary play, and mixing up to 800 ms ahead (a) outran
     * Robot audio, which is primed just-in-time per video frame, baking
     * SILENCE GAPS into a never-dry ring (PCM stutter with zero
     * underruns), and (b) ran script audio-position pacing far ahead of
     * the DAC.  Holding 120 ms keeps mix time tracking the audible
     * stream. */
    const bool pacedPcmActive = _mixer &&
        (_mixer->hasActiveChannelOfType(Audio::Mixer::kSpeechSoundType) ||
         _mixer->hasActiveChannelOfType(Audio::Mixer::kPlainSoundType));
    const int targetPairs = (!pacedPcmActive && of_time_ms() < g_loadCushionUntilMs)
                                ? kLoadCushionPairs : g_targetBufferedPairs;

    /* PCM underrun detector (UART diagnostic, `make debug`).  After every
     * update we know exactly when the ring, left unfed, hits empty:
     * now + buffered/48 ms (48 kHz stereo pairs).  If the NEXT update
     * arrives past that deadline with an empty ring, the DAC sat dry for
     * (now - deadline) ms -- the audible stutter, measured exactly, with
     * the culprit being whatever ran on the main thread in that window.
     * A deadline of 0 means nothing was playing, so silence between
     * sounds never counts.  Rate-limited to one line per second. */
    static uint32 s_dryDeadlineMs = 0;
    static uint32 s_underruns = 0, s_lastUnderrunPrintMs = 0;
    {
        const uint32 nowMs = of_time_ms();
        const int entryFree = of_audio_free();
        int entryBuffered = (_ringCapacity > 0) ? (_ringCapacity - entryFree) : 0;
        if (s_dryDeadlineMs && entryBuffered <= 0 &&
            (int32)(nowMs - s_dryDeadlineMs) > 0) {
            ++s_underruns;
            if ((uint32)(nowMs - s_lastUnderrunPrintMs) >= 1000u) {
                printf("[mixer] UNDERRUN #%u: ring dry ~%ums\n",
                       (unsigned)s_underruns,
                       (unsigned)(nowMs - s_dryDeadlineMs));
                s_lastUnderrunPrintMs = nowMs;
            }
        }
    }
    /* Cap one update() call's work.  When the deep load cushion arms, the
     * uncapped loop mixed the ring from 120 ms straight up to 800 ms in ONE
     * call -- ~200 blocks, ~360 ms of CPU with a rate-converted CDDA stream
     * attached: a hard main-thread freeze (the MI1 CD-music picture stutter).
     * Capped, the cushion still builds just as deep, but spread across the
     * many pump calls a real load generates (one per FS read, >=8 ms apart),
     * so no single call can stall a frame for more than a few blocks' cost. */
    const unsigned kMaxBlocksPerUpdate = 8;
    unsigned blocks = 0;
    while (blocks < kMaxBlocksPerUpdate) {
        int freePairs = of_audio_free();
        if (freePairs < (int)_framesPerBlock)
            break;                                      /* ring full */
        int buffered = (_ringCapacity > 0) ? (_ringCapacity - freePairs) : 0;
        if (buffered < 0)
            buffered = 0;
        if (targetPairs - buffered < (int)_framesPerBlock)
            break;                                      /* cushion reached */
        _mixer->mixCallback((uint8 *)_buffer, _framesPerBlock * 4);
        int wrote = of_audio_write(_buffer, (int)_framesPerBlock);
        blocks++;
        if (wrote <= 0)
            break;
    }

    /* Re-arm the underrun detector's drain deadline from the ring's final
     * depth (48 pairs/ms at 48 kHz).  Zero when the ring is empty and we
     * wrote nothing: nothing is playing, so a later empty ring is not an
     * underrun. */
    {
        int freePairs = of_audio_free();
        int buffered = (_ringCapacity > 0) ? (_ringCapacity - freePairs) : 0;
        s_dryDeadlineMs = (buffered > 0) ? of_time_ms() + (uint32)(buffered / 48)
                                         : 0;
    }
}

void OpenFPGAMixerManager::drain() {
    if (!_initDone)
        return;
    of_mixer_pump();
}

void OpenFPGAMixerManager::suspendAudio() {
    _audioSuspended = true;
}

int OpenFPGAMixerManager::resumeAudio() {
    if (!_audioSuspended) return -2;
    _audioSuspended = false;
    return 0;
}
