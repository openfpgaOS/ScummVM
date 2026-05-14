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
    void update();          /* called from pollEvent — non-blocking */

    void suspendAudio() override;
    int  resumeAudio() override;

private:
    bool      _streamOpen;
    uint32_t  _outputRate;
    uint32_t  _framesPerBlock;
    int16_t  *_buffer;     /* _framesPerBlock * 2 (stereo) int16 */
};

#endif /* OPENFPGA_MIXER_H */
