/*
 * openfpga_mixer.h -- Audio mixer for openfpgaOS
 *
 * Wraps ScummVM's Audio::MixerImpl and pumps PCM samples
 * to the hardware via the of_audio_stream_* API (gapless
 * double-buffered streaming on the new SDK).
 */

#ifndef OPENFPGA_MIXER_H
#define OPENFPGA_MIXER_H

#ifdef __cplusplus
extern "C" {
#endif
#include <of.h>
#include <stdint.h>
#ifdef __cplusplus
}
#endif

#define OPENFPGA_MIX_BUF_SAMPLES 1024

/*
 * OpenFPGAMixer — bridges ScummVM's audio mixer to openfpgaOS PCM output.
 *
 * Will eventually wrap Audio::MixerImpl. For now, manages the PCM
 * ring buffer interface.
 */
class OpenFPGAMixer {
public:
    OpenFPGAMixer();
    ~OpenFPGAMixer();

    void init(int sampleRate);
    void pump();

    int getSampleRate() const { return _sampleRate; }

private:
    int     _sampleRate;
    bool    _streamOpen;
    int16_t _buffer[OPENFPGA_MIX_BUF_SAMPLES * 2];  /* Stereo */
};

#endif /* OPENFPGA_MIXER_H */
