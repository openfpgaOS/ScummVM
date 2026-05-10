/*
 * openfpga_mixer.cpp -- Audio mixer implementation for openfpgaOS
 */

#include "openfpga_mixer.h"

extern "C" {
#include <string.h>
}

OpenFPGAMixer::OpenFPGAMixer() : _sampleRate(0), _streamOpen(false) {
    memset(_buffer, 0, sizeof(_buffer));
}

OpenFPGAMixer::~OpenFPGAMixer() {
    if (_streamOpen) {
        of_audio_stream_close();
        _streamOpen = false;
    }
}

void OpenFPGAMixer::init(int sampleRate) {
    _sampleRate = sampleRate;
    if (!_streamOpen && of_audio_stream_open(sampleRate) >= 0)
        _streamOpen = true;
}

void OpenFPGAMixer::pump() {
    if (!_streamOpen) return;
    /* Only push the next chunk when the stream FIFO is ready for more.
     * Stream buffers are fixed-size on the new SDK -- we always submit
     * a full OPENFPGA_MIX_BUF_SAMPLES block when ready. */
    if (!of_audio_stream_ready()) return;

    /* TODO: Call ScummVM's Audio::MixerImpl::mixCallback() here.
     *   _mixer->mixCallback((uint8_t *)_buffer, OPENFPGA_MIX_BUF_SAMPLES * 4);
     * For now, enqueue silence. */
    memset(_buffer, 0, OPENFPGA_MIX_BUF_SAMPLES * 4);
    of_audio_stream_write(_buffer, OPENFPGA_MIX_BUF_SAMPLES);
}
