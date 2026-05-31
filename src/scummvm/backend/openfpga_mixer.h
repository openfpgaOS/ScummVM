/*
 * openfpga_mixer.h -- ScummVM MixerManager backed by the SDK's
 *                     of_audio_stream_* PCM streaming API.
 *
 * Sample format: interleaved stereo signed 16-bit @ 48 kHz (the
 * SDK's only stream rate, OF_AUDIO_RATE).  The pump pulls mixed
 * frames from Audio::MixerImpl and writes them to the hardware
 * FIFO while it has room (~21 ms = 1024 stereo frames deep).
 *
 * update() is driven from OSystem_OpenFPGA::pollEvent so the audio
 * FIFO is kept topped up at frame rate; mixCallback itself is the
 * expensive part (decodes WMA/MP3/ADPCM into mixed PCM) and is
 * intentionally NOT moved into the timer ISR.
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

    /* PRODUCER (main thread): runs mixCallback into the ring buffer
     * until ~75 % full.  Called from pollEvent / updateScreen and at
     * the start/end of delayMillis sleeps.  Expensive; bursty by
     * design.  Never touches the SDK FIFO directly. */
    void update();

    /* DRAINER (main thread): pushes ring -> SDK FIFO while
     * of_audio_stream_ready() is true.  Cheap when the FIFO is full
     * (single ready() check).  Must be called at fine granularity
     * (~1 ms) so the FIFO never runs dry between mix bursts. */
    void drain();

    /* Legacy name kept for the IRQ-side C shim symbol; currently just
     * calls drain().  The 1 kHz timer ISR does NOT call this because
     * of_audio_stream_write is not re-entrant with the main-thread
     * allocator -- see openfpga_midi.cpp. */
    void drainFromIRQ();

    void suspendAudio() override;
    int  resumeAudio() override;

private:
    bool      _streamOpen;
    uint32_t  _outputRate;
    uint32_t  _framesPerBlock;
    int16_t  *_buffer;     /* _framesPerBlock * 2 (stereo) int16 */

    /* Single-producer (main) / single-consumer (main) ring buffer of
     * already-mixed stereo 16-bit PCM at OF_AUDIO_RATE.
     *
     * 32768 frames = ~683 ms head-room.  Engine stalls during heavy
     * level loads or graphics decodes have been observed up to a
     * few hundred ms; this lets the drainer keep feeding the FIFO
     * straight through them.  Memory cost is 128 KB out of 64 MB
     * SDRAM -- negligible.
     *
     * Must be a power of 2 (head/tail use modulo RING_FRAMES). */
    static const uint32_t RING_FRAMES = 32768;
    int16_t  *_ring;                          /* RING_FRAMES * 2 int16 */
    volatile uint32_t _ringHead;              /* main writes here */
    volatile uint32_t _ringTail;              /* IRQ reads from here */

public:
    /* Diagnostic counters published by drainFromIRQ for the main
     * thread to print.  Not synchronized; treat as approximate. */
    volatile uint32_t _drainCallsForTrace = 0;
    volatile uint32_t _drainPushedForTrace = 0;
    volatile uint32_t _shortWritesForTrace = 0;
    volatile uint32_t _lastChunkForTrace = 0;
    volatile uint32_t _lastWroteForTrace = 0;
};

/* Hook called from the 1 kHz timer IRQ. */
extern "C" void openfpga_mixer_drain_irq(void);

#endif /* OPENFPGA_MIXER_H */
