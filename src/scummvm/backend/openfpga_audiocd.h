/*
 * openfpga_audiocd.h -- AudioCDManager that streams from .cue/.bin pairs.
 *
 * ScummVM games tagged GF_AUDIOTRACKS reference CD audio via track
 * numbers (SOUN type-2 resources).  Upstream's DefaultAudioCDManager
 * looks for pre-converted track%d.ogg/.mp3/.flac files.  This subclass
 * adds a third path: open any .cue file in SearchMan, map track ->
 * (.bin file, byte offset, byte size), and stream the raw 44.1 kHz
 * 16-bit LE stereo PCM directly through the mixer.  Users drop the
 * .cue + matching .bin files into the game zip/ISO and it Just Works
 * -- no offline transcode required.
 */

#ifndef OPENFPGA_AUDIOCD_H
#define OPENFPGA_AUDIOCD_H

#include "backends/audiocd/default/default-audiocd.h"
#include "common/array.h"
#include "common/str.h"

namespace OpenFPGA {
class BufferedCDDAStream;
class HardwareCDDARing;
}

class OpenFPGAAudioCDManager : public DefaultAudioCDManager {
public:
    OpenFPGAAudioCDManager();
    ~OpenFPGAAudioCDManager() override;

    /* Set a constant offset added to every requested track number
     * before lookup.  Lets a compilation disc whose audio is
     * shifted vs the standalone disc (e.g. MI Madness adds an
     * extra audio track at position 2, pushing MI1's audio +1)
     * re-align without engine changes. */
    void setTrackOffset(int offset) { _trackOffset = offset; }

    /* Compatibility no-op: older builds used this to locate zip-backed
     * STORE entries for direct async reads.  CDDA now uses SearchMan
     * streams so compressed zip members and extracted folders behave
     * the same way. */
    void setZipPath(const char *path) {
        (void)path;
    }

    void setCuePath(const char *path) {
        if (path && *path) _cuePath = path;
    }

	bool open() override;

    bool play(int track, int numLoops, int startFrame, int duration,
              bool onlyEmulate = false,
              Audio::Mixer::SoundType soundType = Audio::Mixer::kMusicSoundType) override;
	bool isPlaying() const override;
	void setVolume(byte volume) override;
	void setBalance(int8 balance) override;
	void stop() override;
	void update() override;
	void setPaused(bool paused);

    bool existExtractedCDAudioFiles(uint track) override;

    /* allowBatch=false is the "light" pump for latency-critical call sites:
     * it services the HW cursor/volume but skips the batched blocking refill
     * unless the ring is close to underrun. */
    void pump(bool allowBatch = true);
    void quiesce();

private:
    struct TrackEntry {
        int      number;
        Common::String binFile;   /* path relative to SearchMan root */
        uint32   byteOffset;      /* byte offset of audio start in binFile */
    };

    void loadCueSheet();
    const TrackEntry *findTrack(int track) const;

    Common::Array<TrackEntry> _tracks;
    bool _cueLoaded;
    int  _trackOffset;
	int  _implicitTrackOffset;
	Common::String _cuePath;
	OpenFPGA::BufferedCDDAStream *_activeStream;
	OpenFPGA::HardwareCDDARing *_activeHW;
	bool _paused;
};

extern "C" void openfpga_audiocd_pump(void);
extern "C" void openfpga_audiocd_pump_light(void);
extern "C" void openfpga_audiocd_pause(bool paused);
extern "C" void openfpga_audiocd_quiesce(void);

#endif
