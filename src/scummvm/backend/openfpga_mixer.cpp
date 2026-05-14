/*
 * openfpga_mixer.cpp -- ScummVM MixerManager bridging Audio::MixerImpl
 *                       to the SDK's of_audio_stream_* PCM path.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_mixer.h"

#include "audio/mixer_intern.h"

extern "C" {
#include <stdlib.h>
#include <string.h>
}

OpenFPGAMixerManager::OpenFPGAMixerManager()
    : MixerManager(),
      _streamOpen(false),
      _outputRate(OF_AUDIO_RATE),  /* 48000 -- SDK's fixed stream rate */
      _framesPerBlock(512),        /* ~10.7 ms @ 48 kHz; half the FIFO depth */
      _buffer(nullptr) {}

OpenFPGAMixerManager::~OpenFPGAMixerManager() {
    if (_streamOpen) {
        of_audio_stream_close();
        _streamOpen = false;
    }
    free(_buffer);
}

void OpenFPGAMixerManager::init() {
    /* Audio::MixerImpl(rate, stereo, samples).  `samples` is the
     * granularity at which the mixer will mix -- match our PCM
     * block size so each mixCallback exactly fills the buffer. */
    _mixer = new Audio::MixerImpl(_outputRate, true, _framesPerBlock);
    _mixer->setReady(true);

    /* Two int16 per frame (stereo). */
    _buffer = (int16_t *)calloc(_framesPerBlock * 2, sizeof(int16_t));

    if (of_audio_stream_open((int)_outputRate) >= 0)
        _streamOpen = true;
}

void OpenFPGAMixerManager::update() {
    if (_audioSuspended || !_streamOpen || !_mixer || !_buffer) return;

    /* Top up the SDK FIFO while it has room.  of_audio_stream_ready()
     * returns non-zero when at least one block fits; we keep filling
     * until it goes false so a single pollEvent can recover from a
     * bigger drain (e.g. just after a busy decoder ran). */
    while (of_audio_stream_ready()) {
        _mixer->mixCallback((uint8 *)_buffer, _framesPerBlock);
        if (of_audio_stream_write(_buffer, (int)_framesPerBlock) < 0)
            break;  /* shouldn't happen now that ready() returned true,
                     * but bail rather than spin if the SDK errors. */
    }
}

void OpenFPGAMixerManager::suspendAudio() {
    _audioSuspended = true;
}

int OpenFPGAMixerManager::resumeAudio() {
    if (!_audioSuspended) return -2;
    _audioSuspended = false;
    return 0;
}
