/*
 * openfpga_audiocd.cpp -- AudioCDManager streaming raw CD audio from
 *                          a .cue/.bin pair via SearchMan.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_audiocd.h"

#include "audio/audiostream.h"
#include "audio/mixer.h"
#include "common/archive.h"
#include "common/debug.h"
#include "common/substream.h"
#include "common/textconsole.h"

extern "C" {
#include <of_cache.h>
#include <of_file.h>
#include <of_mixer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
}

namespace {

/* CD audio rate / format are fixed by the Red Book spec. */
constexpr int kCDRate        = 44100;
constexpr int kBytesPerFrame = 2352;   /* sector = 1/75 s of stereo 16-bit PCM */
constexpr uint32 kBytesPerStereoFrame = 4;
constexpr uint32 kCDRingBytes = 512 * 1024;
constexpr uint32 kCDPrimeBytes = 192 * 1024;
constexpr uint32 kCDSyncReadBytes = 16 * 1024;
constexpr uint32 kCDAsyncReadBytes = 64 * 1024;
constexpr uint32 kCDHWFrames = 65536;       /* ~1.5 s at 44.1 kHz */
constexpr uint32 kCDHWChunkFrames = 4096;   /* ~93 ms refill unit */
constexpr uint32 kCDHWGuardFrames = 512;
constexpr uint32 kCDHWSilenceFrames = 2048;
/* Refill hysteresis.  Topping the ring up on every pump tick reads ~800 bytes
 * ~200x per second -- each one a blocking APF-bridge round-trip on the render
 * thread (the CD-music picture stutter).  Instead let the ring drain a full
 * batch and refill it in a few large reads (~5x per second).  Light pump sites
 * (mid-present, mid-read) don't batch at all; they only refill below the
 * emergency floor so a long non-yielding engine stretch can't underrun. */
constexpr uint32 kCDHWRefillBatchFrames = 8192;   /* ~186 ms per batch */
constexpr uint32 kCDHWEmergencyFrames   = 16384;  /* ~371 ms floor */

/* Scoped re-entrancy flag for the pump paths (they have several exits). */
struct PumpScope {
    explicit PumpScope(bool &flag) : _flag(flag) { _flag = true; }
    ~PumpScope() { _flag = false; }
    bool &_flag;
};

class StdioSeekableReadStream : public Common::SeekableReadStream {
public:
    explicit StdioSeekableReadStream(FILE *f) : _f(f), _size(0) {
        if (_f && fseeko(_f, 0, SEEK_END) == 0) {
            _size = (int64)ftello(_f);
            fseeko(_f, 0, SEEK_SET);
        }
    }
    ~StdioSeekableReadStream() override { if (_f) fclose(_f); }

    bool eos() const override { return feof(_f) != 0; }
    bool err() const override { return ferror(_f) != 0; }
    void clearErr() override { clearerr(_f); }
    int64 pos() const override { return (int64)ftello(_f); }
    int64 size() const override { return _size; }
    uint32 read(void *buf, uint32 cnt) override {
        return (uint32)fread(buf, 1, cnt, _f);
    }
    bool seek(int64 offset, int whence = SEEK_SET) override {
        return fseeko(_f, (off_t)offset, whence) == 0;
    }

private:
    FILE *_f;
    int64 _size;
};

const char *baseNameOf(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *backslash = strrchr(name, '\\');
    const char *base = slash;
    if (!base || (backslash && backslash > base))
        base = backslash;
    return base ? base + 1 : name;
}

bool hasDirectoryComponent(const char *name) {
    return strchr(name, '/') || strchr(name, '\\');
}

bool hasCueExtension(const Common::String &name) {
    const char *s = name.c_str();
    size_t len = strlen(s);
    return len >= 4 && strcasecmp(s + len - 4, ".cue") == 0;
}

Common::String directoryOf(const Common::String &path) {
    const char *s = path.c_str();
    const char *slash = strrchr(s, '/');
    const char *backslash = strrchr(s, '\\');
    const char *last = slash;
    if (!last || (backslash && backslash > last))
        last = backslash;
    if (!last)
        return Common::String();
    return Common::String(s, (uint)(last - s + 1));
}

Common::SeekableReadStream *openNamedStreamOnce(const Common::String &name) {
    Common::ArchiveMemberPtr m = SearchMan.getMember(Common::Path(name));
    if (m)
        return m->createReadStream();

    FILE *f = fopen(name.c_str(), "rb");
    if (f)
        return new StdioSeekableReadStream(f);

    return nullptr;
}

Common::SeekableReadStream *openNamedStream(const Common::String &name,
                                            const Common::String &cuePath = Common::String()) {
    if (name.empty())
        return nullptr;

    Common::SeekableReadStream *stream = openNamedStreamOnce(name);
    if (stream)
        return stream;

    if (!cuePath.empty() && !hasDirectoryComponent(name.c_str())) {
        Common::String dir = directoryOf(cuePath);
        if (!dir.empty()) {
            stream = openNamedStreamOnce(dir + name);
            if (stream)
                return stream;
        }
    }

    const char *base = baseNameOf(name.c_str());
    if (base != name.c_str())
        return openNamedStreamOnce(Common::String(base));

    return nullptr;
}

/* Open any .cue SearchMan can find.  Goes through ArchiveMember to
 * avoid Common::File path-encoding pitfalls on names with spaces or
 * parens (e.g. "Monkey Island Madness (USA).cue"). */
Common::SeekableReadStream *openAnyCueStream() {
    Common::ArchiveMemberList list;
    SearchMan.listMatchingMembers(list, "*.cue");
    if (!list.empty()) {
        Common::String name = list.front()->getName();
        debug(1, "[audiocd] cue: %s", name.c_str());
        return list.front()->createReadStream();
    }
    return nullptr;
}

OpenFPGAAudioCDManager *g_activeCDManager = nullptr;

char *skipSpaces(char *s) {
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

void trimLine(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' ||
                     e[-1] == ' ' || e[-1] == '\t'))
        --e;
    *e = '\0';
}

bool startsWithI(const char *s, const char *prefix) {
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

bool parseCueFileName(const char *line, char *out, size_t cap) {
    const char *p = line + 4; /* FILE */
    while (*p == ' ' || *p == '\t')
        ++p;
    if (!*p || cap == 0)
        return false;

    size_t n = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && n + 1 < cap)
            out[n++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && n + 1 < cap)
            out[n++] = *p++;
    }
    out[n] = '\0';
    return n != 0;
}

bool parseCueTrack(const char *line, int &number, char *type, size_t typeCap) {
    const char *p = line + 5; /* TRACK */
    while (*p == ' ' || *p == '\t')
        ++p;
    char localType[32];
    if (sscanf(p, "%d %31s", &number, localType) != 2)
        return false;
    snprintf(type, typeCap, "%s", localType);
    return true;
}

bool parseCueIndex(const char *line, int &indexNo, int &frame) {
    const char *p = line + 5; /* INDEX */
    int mm = 0, ss = 0, ff = 0;
    if (sscanf(p, "%d %d:%d:%d", &indexNo, &mm, &ss, &ff) != 4)
        return false;
    frame = ((mm * 60) + ss) * 75 + ff;
    return frame >= 0;
}

int findDataSlot(const Common::String &name) {
    if (name.empty())
        return -1;

    uint32_t slot = 0;
    if (of_file_slot_find(name.c_str(), &slot) == 0)
        return (int)slot;

    const char *base = baseNameOf(name.c_str());
    if (base != name.c_str() && of_file_slot_find(base, &slot) == 0)
        return (int)slot;

    return -1;
}

} // namespace

namespace OpenFPGA {

class BufferedCDDAStream;

volatile BufferedCDDAStream *g_cddaAsyncOwner = nullptr;
volatile bool g_cddaAsyncDone = false;
volatile int g_cddaAsyncResult = 0;

void cddaAsyncCallback(int token, int result) {
    (void)token;
    if (g_cddaAsyncOwner) {
        g_cddaAsyncResult = result;
        g_cddaAsyncDone = true;
    }
}

class BufferedCDDAStream : public Audio::SeekableAudioStream {
public:
    BufferedCDDAStream(Common::SeekableReadStream *syncStream,
                       uint32 streamBytes, int numLoops,
                       int asyncSlot, uint32 asyncBaseOffset)
        : _syncStream(syncStream), _streamBytes(streamBytes & ~3u), _sourcePos(0),
          _asyncSlot(asyncSlot), _asyncBaseOffset(asyncBaseOffset),
          _asyncChunk(0), _asyncPending(false), _pendingBytes(0),
          _ring(nullptr), _ringSize(kCDRingBytes), _head(0), _tail(0), _used(0),
          _initialLoops((numLoops < 0) ? 0u : (uint)((numLoops <= 1) ? 1 : numLoops)),
          _loopsLeftAfterCurrent(0), _infinite(numLoops < 0), _finished(false),
          _err(false), _inPump(false) {
        _ring = (byte *)malloc(_ringSize);
        if (_asyncSlot >= 0) {
            _asyncChunk = of_file_async_max_read();
            if (_asyncChunk > kCDAsyncReadBytes)
                _asyncChunk = kCDAsyncReadBytes;
            _asyncChunk &= ~3u;
            if (_asyncChunk < kBytesPerStereoFrame)
                _asyncSlot = -1;
        }
        resetLoops();
        if (!_syncStream || !_ring)
            _err = true;
    }

    ~BufferedCDDAStream() override {
        waitForAsync();
        delete _syncStream;
        free(_ring);
    }

    bool prime() {
        if (_err || !_ring)
            return false;
        uint32 target = _streamBytes < kCDPrimeBytes ? _streamBytes : kCDPrimeBytes;
        while (!_finished && _used < target && !_err) {
            uint32 before = _used;
            pump(true);
            if (_used == before)
                break;
        }
        return !_err && _used != 0;
    }

    void pump(bool wait) {
        /* Re-entered via a mid-read mixer pump (see HardwareCDDARing::pump);
         * a nested read would clobber _head/_sourcePos mid-transfer. */
        if (_inPump)
            return;
        if (_err || _finished || !_ring)
            return;
        PumpScope scope(_inPump);

        pollAsync();
        if (wait)
            waitForAsync();

        uint32 issued = 0;
        while (ringFree() >= kBytesPerStereoFrame && !_finished && !_err) {
            if (_sourcePos >= _streamBytes) {
                if (!advanceLoop())
                    break;
            }

            uint32 remaining = _streamBytes - _sourcePos;
            uint32 freeBytes = ringFree();
            if (remaining == 0 || freeBytes < kBytesPerStereoFrame)
                break;

            uint32 contiguous = contiguousWrite();
            uint32 want = remaining < contiguous ? remaining : contiguous;
            uint32 maxRead = (_asyncSlot >= 0) ? _asyncChunk : kCDSyncReadBytes;
            if (want > maxRead)
                want = maxRead;
            want &= ~3u;
            if (want == 0)
                break;

            if (_asyncSlot >= 0) {
                if (!issueAsync(want)) {
                    if (!wait)
                        break;
                    if (!readSyncChunk(want))
                        break;
                }
                if (wait)
                    waitForAsync();
                ++issued;
            } else {
                if (!readSyncChunk(want))
                    break;
                ++issued;
            }

            if (!wait && issued != 0)
                break;
        }
    }

    int readBuffer(int16 *buffer, const int numSamples) override {
        if (numSamples <= 0)
            return 0;
        uint32 want = (uint32)numSamples * sizeof(int16);
        uint32 copied = 0;
        byte *out = (byte *)buffer;

        while (want) {
            if (_used == 0 && !_finished && !_err) {
                pump(false);
                if (_used == 0)
                    refillBlocking();
            }

            if (_used == 0)
                break;

            uint32 contiguous = contiguousRead();
            uint32 take = contiguous < want ? contiguous : want;
            memcpy(out, _ring + _tail, take);
            _tail = (_tail + take) % _ringSize;
            _used -= take;
            out += take;
            copied += take;
            want -= take;
        }

        if (want) {
            if ((_finished || _err) && copied == 0)
                return 0;

            memset(out, 0, want);
            copied += want;
        }

        return copied / sizeof(int16);
    }

    bool isStereo() const override { return true; }
    int getRate() const override { return kCDRate; }
    bool endOfData() const override { return _finished && _used == 0; }
    bool endOfStream() const override { return endOfData(); }

    bool seek(const Audio::Timestamp &where) override {
        waitForAsync();
        Audio::Timestamp ts = where.convertToFramerate(kCDRate);
        int frames = ts.totalNumberOfFrames();
        if (frames < 0)
            return false;
        uint32 bytePos = (uint32)frames * kBytesPerStereoFrame;
        if (bytePos > _streamBytes)
            return false;

        _head = _tail = _used = 0;
        _sourcePos = bytePos & ~3u;
        _finished = false;
        _err = false;
        resetLoops();
        if (_syncStream && !_syncStream->seek(_sourcePos))
            return false;
        pump(true);
        return true;
    }

    void quiesce() {
        waitForAsync();
    }

    Audio::Timestamp getLength() const override {
        return Audio::Timestamp(0, _streamBytes / kBytesPerStereoFrame, kCDRate);
    }

private:
    bool refillBlocking() {
        if (_inPump)                 /* nested via a mid-read mixer pump */
            return false;
        if (_err || _finished || !_ring)
            return false;
        PumpScope scope(_inPump);

        waitForAsync();
        if (_sourcePos >= _streamBytes && !advanceLoop())
            return false;

        uint32 remaining = _streamBytes - _sourcePos;
        uint32 freeBytes = ringFree();
        if (remaining == 0 || freeBytes < kBytesPerStereoFrame)
            return false;

        uint32 contiguous = contiguousWrite();
        uint32 want = remaining < contiguous ? remaining : contiguous;
        uint32 maxRead = (_asyncSlot >= 0) ? _asyncChunk : kCDSyncReadBytes;
        if (want > maxRead)
            want = maxRead;
        want &= ~3u;
        if (want == 0)
            return false;

        if (_asyncSlot >= 0 && issueAsync(want)) {
            waitForAsync();
            return !_err && _used != 0;
        }

        return readSyncChunk(want);
    }

    bool readSyncChunk(uint32 want) {
        uint32 got = _syncStream ? _syncStream->read(_ring + _head, want) : 0;
        got &= ~3u;
        if (got == 0) {
            _err = _syncStream && _syncStream->err();
            if (_err)
                _finished = true;
            _sourcePos = _streamBytes;
            return false;
        }

        commitWrite(got);
        _sourcePos += got;
        return true;
    }

    bool issueAsync(uint32 want) {
        if (_asyncSlot < 0 || _asyncPending || want == 0)
            return false;
        if (of_file_async_busy() > 0)
            return false;

        g_cddaAsyncOwner = this;
        g_cddaAsyncDone = false;
        g_cddaAsyncResult = 0;

        int token = of_file_read_async(_asyncSlot, _asyncBaseOffset + _sourcePos,
                                       _ring + _head, want, cddaAsyncCallback);
        if (token < 0) {
            g_cddaAsyncOwner = nullptr;
            _asyncSlot = -1;
            return false;
        }

        _asyncPending = true;
        _pendingBytes = want;
        return true;
    }

    void pollAsync() {
        if (!_asyncPending)
            return;

        of_file_async_poll();
        if (g_cddaAsyncOwner != this || !g_cddaAsyncDone)
            return;

        int result = g_cddaAsyncResult;
        g_cddaAsyncOwner = nullptr;
        g_cddaAsyncDone = false;
        g_cddaAsyncResult = 0;
        _asyncPending = false;

        if (result < 0) {
            _err = true;
            _finished = true;
            _pendingBytes = 0;
            return;
        }

        commitWrite(_pendingBytes);
        _sourcePos += _pendingBytes;
        _pendingBytes = 0;
    }

    void waitForAsync() {
        while (_asyncPending) {
            pollAsync();
            if (_asyncPending)
                of_file_async_poll();
        }
    }

    void resetLoops() {
        _loopsLeftAfterCurrent = (_infinite || _initialLoops == 0) ? 0 : _initialLoops - 1;
    }

    bool advanceLoop() {
        if (_streamBytes == 0) {
            _finished = true;
            return false;
        }

        if (!_infinite) {
            if (_loopsLeftAfterCurrent == 0) {
                _finished = true;
                return false;
            }
            --_loopsLeftAfterCurrent;
        }

        _sourcePos = 0;
        if (_syncStream && !_syncStream->seek(0)) {
            _err = true;
            _finished = true;
            return false;
        }
        return true;
    }

    uint32 ringFree() const { return _ringSize - _used; }

    uint32 contiguousWrite() const {
        uint32 freeBytes = ringFree();
        if (!freeBytes)
            return 0;
        uint32 span = (_head >= _tail) ? (_ringSize - _head) : (_tail - _head);
        return span < freeBytes ? span : freeBytes;
    }

    uint32 contiguousRead() const {
        if (!_used)
            return 0;
        uint32 span = (_tail >= _head) ? (_ringSize - _tail) : (_head - _tail);
        return span < _used ? span : _used;
    }

    void commitWrite(uint32 bytes) {
        _head = (_head + bytes) % _ringSize;
        _used += bytes;
    }

    Common::SeekableReadStream *_syncStream;
    uint32 _streamBytes;
    uint32 _sourcePos;
    int _asyncSlot;
    uint32 _asyncBaseOffset;
    uint32 _asyncChunk;
    bool _asyncPending;
    uint32 _pendingBytes;

    byte *_ring;
    uint32 _ringSize;
    uint32 _head;
    uint32 _tail;
    uint32 _used;

    uint _initialLoops;
    uint _loopsLeftAfterCurrent;
    bool _infinite;
    bool _finished;
    bool _err;
    bool _inPump;
};

class HardwareCDDARing {
public:
    HardwareCDDARing(Common::SeekableReadStream *stream,
                     uint32 streamBytes, int numLoops,
                     Audio::Mixer *mixer, Audio::Mixer::SoundType soundType,
                     byte volume, int8 balance)
        : _stream(stream), _streamBytes(streamBytes & ~3u), _sourcePos(0),
          _left(nullptr), _right(nullptr), _io(nullptr),
          _ringFrames(kCDHWFrames), _writtenFrames(0), _playedFrames(0),
          _lastHWPos(0), _haveLastHWPos(false),
          _leftVoice(OF_MIXER_HANDLE_INVALID),
          _rightVoice(OF_MIXER_HANDLE_INVALID),
          _initialLoops((numLoops < 0) ? 0u : (uint)((numLoops <= 1) ? 1 : numLoops)),
          _loopsLeftAfterCurrent(0), _infinite(numLoops < 0),
          _tailSilenceWritten(false), _finishedFeeding(false),
          _started(false), _paused(false), _done(false), _err(false),
          _inPump(false),
          _mixer(mixer), _soundType(soundType), _volume(volume),
          _balance(balance), _lastVolL(-1), _lastVolR(-1) {
        _left = (int16 *)calloc(_ringFrames, sizeof(int16));
        _right = (int16 *)calloc(_ringFrames, sizeof(int16));
        _io = (byte *)malloc(kCDHWChunkFrames * kBytesPerStereoFrame);
        resetLoops();
        if (!_stream || !_left || !_right || !_io || _streamBytes == 0)
            _err = true;
    }

    ~HardwareCDDARing() {
        stop();
        delete _stream;
        free(_left);
        free(_right);
        free(_io);
    }

    bool start() {
        if (_err)
            return false;

        uint32 primed = fillAvailable(_ringFrames - kCDHWGuardFrames);
        if (primed == 0)
            return false;

        of_cache_flush_range(_left, _ringFrames * sizeof(int16));
        of_cache_flush_range(_right, _ringFrames * sizeof(int16));

        _leftVoice = of_mixer_alloc_for_group_h(OF_MIXER_GROUP_AUX,
                                                (const uint8_t *)_left,
                                                _ringFrames, kCDRate, 2, 0);
        _rightVoice = of_mixer_alloc_for_group_h(OF_MIXER_GROUP_AUX,
                                                 (const uint8_t *)_right,
                                                 _ringFrames, kCDRate, 2, 0);
        if (_leftVoice == OF_MIXER_HANDLE_INVALID ||
            _rightVoice == OF_MIXER_HANDLE_INVALID) {
            stop();
            return false;
        }

        of_mixer_set_loop_h(_leftVoice, 0, _ringFrames);
        of_mixer_set_loop_h(_rightVoice, 0, _ringFrames);
        of_mixer_set_rate_raw_h(_leftVoice, OF_MIXER_RATE_FP16(kCDRate));
        of_mixer_set_rate_raw_h(_rightVoice, OF_MIXER_RATE_FP16(kCDRate));
        applyVolume();

        int pos = of_mixer_get_position_h(_leftVoice);
        _lastHWPos = (pos >= 0) ? (uint32)pos % _ringFrames : 0;
        _haveLastHWPos = true;
        _started = true;
        return true;
    }

    void stop() {
        if (_leftVoice != OF_MIXER_HANDLE_INVALID)
            of_mixer_stop_h(_leftVoice);
        if (_rightVoice != OF_MIXER_HANDLE_INVALID)
            of_mixer_stop_h(_rightVoice);
        _leftVoice = OF_MIXER_HANDLE_INVALID;
        _rightVoice = OF_MIXER_HANDLE_INVALID;
        _started = false;
    }

    void pump(bool allowBatch) {
        /* Re-entered via a mid-read mixer pump (SlotFileStream pumps audio
         * between zip read chunks; without the guard that nested pump could
         * issue a second read into _io while the outer one is in flight). */
        if (_inPump)
            return;
        if (!_started || _paused || _done)
            return;
        PumpScope scope(_inPump);

        updateCursor();
        if (!_started || _done)
            return;

        if (!of_mixer_handle_active(_leftVoice) ||
            !of_mixer_handle_active(_rightVoice)) {
            stop();
            _done = true;
            return;
        }

        applyVolume();

        if (!_finishedFeeding) {
            uint64 buffered = bufferedFrames();
            uint32 freeFrames = 0;
            if (buffered + kCDHWGuardFrames < _ringFrames)
                freeFrames = _ringFrames - (uint32)buffered - kCDHWGuardFrames;
            const bool urgent = buffered < kCDHWEmergencyFrames;
            if (urgent || (allowBatch && freeFrames >= kCDHWRefillBatchFrames)) {
                /* Cap one pump's worth of blocking reads; after a long stall
                 * the next pumps (urgent) top the rest back up. */
                uint32 budget = freeFrames;
                if (budget > kCDHWRefillBatchFrames * 2)
                    budget = kCDHWRefillBatchFrames * 2;
                fillAvailable(budget);
            }
        }
        updateCursor();
    }

    bool isActive() const {
        if (_done)
            return false;
        if (_started)
            return of_mixer_handle_active(_leftVoice) &&
                   of_mixer_handle_active(_rightVoice);
        return false;
    }

    void setPaused(bool paused) {
        if (!_started || _paused == paused)
            return;
        updateCursor();
        _paused = paused;
        uint32 rate = paused ? 0u : OF_MIXER_RATE_FP16(kCDRate);
        of_mixer_set_rate_raw_h(_leftVoice, rate);
        of_mixer_set_rate_raw_h(_rightVoice, rate);
    }

    void setVolume(byte volume, int8 balance) {
        _volume = volume;
        _balance = balance;
        applyVolume();
    }

    void quiesce() {
        updateCursor();
    }

private:
    void resetLoops() {
        _loopsLeftAfterCurrent = (_infinite || _initialLoops == 0) ? 0 : _initialLoops - 1;
    }

    bool advanceLoop() {
        if (_streamBytes == 0)
            return false;
        if (!_infinite) {
            if (_loopsLeftAfterCurrent == 0)
                return false;
            --_loopsLeftAfterCurrent;
        }
        _sourcePos = 0;
        return _stream && _stream->seek(0);
    }

    uint64 bufferedFrames() const {
        return (_writtenFrames > _playedFrames) ? (_writtenFrames - _playedFrames) : 0;
    }

    void updateCursor() {
        if (!_started || !_haveLastHWPos)
            return;

        int posRaw = of_mixer_get_position_h(_leftVoice);
        if (posRaw < 0)
            return;
        uint32 pos = (uint32)posRaw % _ringFrames;
        uint32 delta = (pos >= _lastHWPos)
                       ? (pos - _lastHWPos)
                       : (_ringFrames - _lastHWPos + pos);
        _lastHWPos = pos;
        if (delta == 0)
            return;

        uint64 buffered = bufferedFrames();
        if ((uint64)delta > buffered)
            delta = (uint32)buffered;
        _playedFrames += delta;

        if (_finishedFeeding && bufferedFrames() == 0) {
            stop();
            _done = true;
        }
    }

    uint32 fillAvailable(uint32 frameBudget) {
        uint32 filled = 0;
        while (!_finishedFeeding && frameBudget > 0) {
            uint64 buffered = bufferedFrames();
            if (buffered + kCDHWGuardFrames >= _ringFrames)
                break;

            uint32 freeFrames = _ringFrames - (uint32)buffered - kCDHWGuardFrames;
            uint32 dst = (uint32)(_writtenFrames % _ringFrames);
            uint32 contiguous = _ringFrames - dst;
            uint32 want = freeFrames;
            if (want > contiguous)
                want = contiguous;
            if (want > frameBudget)
                want = frameBudget;
            if (want > kCDHWChunkFrames)
                want = kCDHWChunkFrames;
            if (want == 0)
                break;

            uint32 got = readAndWrite(dst, want);
            if (got == 0) {
                if (!_tailSilenceWritten) {
                    uint32 silence = freeFrames;
                    if (silence > contiguous)
                        silence = contiguous;
                    if (silence > kCDHWSilenceFrames)
                        silence = kCDHWSilenceFrames;
                    if (silence == 0)
                        break;
                    writeSilence(dst, silence);
                    _writtenFrames += silence;
                    _tailSilenceWritten = true;
                    _finishedFeeding = true;
                    filled += silence;
                    break;
                }
                _finishedFeeding = true;
                break;
            }

            _writtenFrames += got;
            frameBudget -= got;
            filled += got;
        }
        return filled;
    }

    uint32 readAndWrite(uint32 dstFrame, uint32 maxFrames) {
        while (_sourcePos >= _streamBytes) {
            if (!advanceLoop())
                return 0;
        }

        uint32 remainingFrames = (_streamBytes - _sourcePos) / kBytesPerStereoFrame;
        uint32 wantFrames = maxFrames;
        if (wantFrames > remainingFrames)
            wantFrames = remainingFrames;
        if (wantFrames > kCDHWChunkFrames)
            wantFrames = kCDHWChunkFrames;
        if (wantFrames == 0)
            return 0;

        uint32 wantBytes = wantFrames * kBytesPerStereoFrame;
        uint32 gotBytes = _stream ? _stream->read(_io, wantBytes) : 0;
        gotBytes &= ~3u;
        if (gotBytes == 0) {
            _err = _stream && _stream->err();
            _sourcePos = _streamBytes;
            return 0;
        }

        uint32 frames = gotBytes / kBytesPerStereoFrame;
        const int16 *src = (const int16 *)_io;
        for (uint32 i = 0; i < frames; ++i) {
            _left[dstFrame + i] = src[i * 2 + 0];
            _right[dstFrame + i] = src[i * 2 + 1];
        }

        flushWritten(dstFrame, frames);
        _sourcePos += gotBytes;
        return frames;
    }

    void writeSilence(uint32 dstFrame, uint32 frames) {
        memset(_left + dstFrame, 0, frames * sizeof(int16));
        memset(_right + dstFrame, 0, frames * sizeof(int16));
        flushWritten(dstFrame, frames);
    }

    void flushWritten(uint32 dstFrame, uint32 frames) {
        of_cache_flush_range(_left + dstFrame, frames * sizeof(int16));
        of_cache_flush_range(_right + dstFrame, frames * sizeof(int16));
    }

    void applyVolume() {
        if (_leftVoice == OF_MIXER_HANDLE_INVALID ||
            _rightVoice == OF_MIXER_HANDLE_INVALID)
            return;

        int base = 0;
        if (_mixer && !_mixer->isSoundTypeMuted(_soundType)) {
            base = (_mixer->getVolumeForSoundType(_soundType) * _volume) /
                   Audio::Mixer::kMaxMixerVolume;
        }
        if (base < 0) base = 0;
        if (base > 255) base = 255;

        int vl = base;
        int vr = base;
        if (_balance < 0)
            vr = ((127 + _balance) * base) / 127;
        else if (_balance > 0)
            vl = ((127 - _balance) * base) / 127;
        if (vl < 0) vl = 0;
        if (vr < 0) vr = 0;
        if (vl > 255) vl = 255;
        if (vr > 255) vr = 255;

        if (vl == _lastVolL && vr == _lastVolR)
            return;

        of_mixer_set_vol_lr_h(_leftVoice, vl, 0);
        of_mixer_set_vol_lr_h(_rightVoice, 0, vr);
        _lastVolL = vl;
        _lastVolR = vr;
    }

    Common::SeekableReadStream *_stream;
    uint32 _streamBytes;
    uint32 _sourcePos;
    int16 *_left;
    int16 *_right;
    byte *_io;
    uint32 _ringFrames;
    uint64 _writtenFrames;
    uint64 _playedFrames;
    uint32 _lastHWPos;
    bool _haveLastHWPos;
    of_mixer_handle_t _leftVoice;
    of_mixer_handle_t _rightVoice;
    uint _initialLoops;
    uint _loopsLeftAfterCurrent;
    bool _infinite;
    bool _tailSilenceWritten;
    bool _finishedFeeding;
    bool _started;
    bool _paused;
    bool _done;
    bool _err;
    bool _inPump;
    Audio::Mixer *_mixer;
    Audio::Mixer::SoundType _soundType;
    byte _volume;
    int8 _balance;
    int _lastVolL;
    int _lastVolR;
};

} // namespace OpenFPGA

OpenFPGAAudioCDManager::OpenFPGAAudioCDManager()
    : DefaultAudioCDManager(), _cueLoaded(false), _trackOffset(0),
      _implicitTrackOffset(0), _activeStream(nullptr), _activeHW(nullptr),
      _paused(false) {
    /* Defer loadCueSheet() to open(): the ctor runs from
     * OSystem::initBackend() before main.cpp adds the game zip to
     * SearchMan, so the cue is invisible here. */
    g_activeCDManager = this;
}

OpenFPGAAudioCDManager::~OpenFPGAAudioCDManager() {
    stop();
    if (g_activeCDManager == this)
        g_activeCDManager = nullptr;
}

bool OpenFPGAAudioCDManager::open() {
    /* DefaultAudioCDManager::open() just stops + returns true; we use
     * the call as a one-shot hook to scan for a cue once the engine
     * has populated SearchMan. */
    bool ok = DefaultAudioCDManager::open();
    if (!_cueLoaded && _tracks.empty())
        loadCueSheet();
    return ok;
}

void OpenFPGAAudioCDManager::loadCueSheet() {
    _implicitTrackOffset = 0;
    if (!_cuePath.empty() && !hasCueExtension(_cuePath)) {
        debug(1, "[audiocd] ignoring non-cue path '%s'", _cuePath.c_str());
        _cueLoaded = true;
        return;
    }

    Common::SeekableReadStream *cueStream = openNamedStream(_cuePath);
    if (!cueStream)
        cueStream = openAnyCueStream();
    if (!cueStream) {
        debug(1, "[audiocd] no .cue file found -- CDDA unavailable");
        return;
    }

    uint32 sz = (uint32)cueStream->size();
    char *cueText = new char[sz + 1];
    uint32 got = cueStream->read(cueText, sz);
    cueText[got] = '\0';
    delete cueStream;

    char currentFile[256] = "";
    bool pendingAudio = false;
    int pendingNumber = 0;
    int pendingIndex0 = -1;
    int pendingIndex1 = -1;
    Common::String pendingFile;

    auto commitPending = [&]() {
        if (!pendingAudio)
            return;
        int frame = pendingIndex1 >= 0 ? pendingIndex1 : pendingIndex0;
        if (frame < 0 || pendingFile.empty())
            return;
        TrackEntry e;
        e.number     = pendingNumber;
        e.binFile    = pendingFile;
        e.byteOffset = (uint32)frame * kBytesPerFrame;
        _tracks.push_back(e);

        debug(1, "[audiocd] track %d -> '%s' @+%u",
              e.number, e.binFile.c_str(), e.byteOffset);
    };

    char *line = cueText;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        trimLine(line);
        char *p = skipSpaces(line);

        if (startsWithI(p, "FILE")) {
            parseCueFileName(p, currentFile, sizeof(currentFile));
        } else if (startsWithI(p, "TRACK")) {
            commitPending();
            pendingAudio = false;
            pendingNumber = 0;
            pendingIndex0 = -1;
            pendingIndex1 = -1;
            pendingFile.clear();

            char type[32];
            int number = 0;
            if (parseCueTrack(p, number, type, sizeof(type)) &&
                strcasecmp(type, "AUDIO") == 0) {
                pendingAudio = true;
                pendingNumber = number;
                pendingFile = currentFile;
            }
        } else if (startsWithI(p, "INDEX")) {
            int indexNo = -1;
            int frame = -1;
            if (pendingAudio && parseCueIndex(p, indexNo, frame)) {
                if (indexNo == 0)
                    pendingIndex0 = frame;
                else if (indexNo == 1)
                    pendingIndex1 = frame;
            }
        }

        line = next;
    }
    commitPending();
    delete[] cueText;

    _cueLoaded = !_tracks.empty();
    if (!_cueLoaded)
        warning("[audiocd] cue parsed but no audio tracks found");
    else if (!findTrack(1) && findTrack(2)) {
        _implicitTrackOffset = 1;
        debug(1, "[audiocd] cue has data track 1; logical audio tracks remap +1");
    }
}

const OpenFPGAAudioCDManager::TrackEntry *
OpenFPGAAudioCDManager::findTrack(int track) const {
    for (uint i = 0; i < _tracks.size(); ++i) {
        if (_tracks[i].number == track)
            return &_tracks[i];
    }
    return nullptr;
}

bool OpenFPGAAudioCDManager::play(int track, int numLoops, int startFrame,
                                  int duration, bool onlyEmulate,
                                  Audio::Mixer::SoundType soundType) {
    /* Lazy-load if open() wasn't called (e.g. games without
     * GF_AUDIOTRACKS that still call play() opportunistically). */
    if (!_cueLoaded && _tracks.empty())
        loadCueSheet();
    if (!_cueLoaded)
        return DefaultAudioCDManager::play(track, numLoops, startFrame,
                                            duration, onlyEmulate, soundType);

    int appliedOffset = _trackOffset;
    int physTrack = track + appliedOffset;
    const TrackEntry *e = findTrack(physTrack);
    if (!e && appliedOffset == 0 && _implicitTrackOffset != 0) {
        int implicitTrack = track + _implicitTrackOffset;
        const TrackEntry *implicitEntry = findTrack(implicitTrack);
        if (implicitEntry) {
            appliedOffset = _implicitTrackOffset;
            physTrack = implicitTrack;
            e = implicitEntry;
        }
    }
    if (appliedOffset)
        debug(1, "[audiocd] track remap %d -> %d (offset %d)",
              track, physTrack, appliedOffset);
    if (!e && _trackOffset == 0 && track == 1 &&
        _tracks.size() == 1 && _tracks[0].number > 1) {
        physTrack = _tracks[0].number;
        e = &_tracks[0];
        debug(1, "[audiocd] track auto-remap %d -> %d (single audio track)",
              track, physTrack);
    }
    if (!e)
        return DefaultAudioCDManager::play(track, numLoops, startFrame,
                                            duration, onlyEmulate, soundType);

    /* Stop any previous emulated track first; DefaultAudioCDManager owns
     * _handle/_emulating so we delegate that bit. */
    stop();

    /* Open the .bin through SearchMan so extracted folders, ISO mounts,
     * and compressed zip members all follow the same stream path. */
    Common::SeekableReadStream *bin = nullptr;
    bin = openNamedStream(e->binFile, _cuePath);
    if (!bin) {
        warning("[audiocd] failed to open '%s' for track %d",
                e->binFile.c_str(), track);
        return false;
    }

    uint32 binSize = (uint32)bin->size();
    if (startFrame < 0)
        startFrame = 0;

    /* Compute byte window: skip pregap + caller's startFrame, cap to
     * `duration` frames if specified, else stop at the next audio track
     * in the same .bin.  Caller frames are 1/75-sec sectors relative to
     * track start. */
    uint32 startByte = e->byteOffset + (uint32)startFrame * kBytesPerFrame;
    if (startByte >= binSize) {
        warning("[audiocd] track %d: startByte %u beyond file size %u",
                track, startByte, binSize);
        delete bin;
        return false;
    }

    uint32 endByte = binSize;
    for (uint i = 0; i < _tracks.size(); ++i) {
        const TrackEntry &next = _tracks[i];
        if (next.byteOffset > e->byteOffset &&
            next.byteOffset < endByte &&
            next.binFile.equalsIgnoreCase(e->binFile))
            endByte = next.byteOffset;
    }

    if (duration > 0) {
        uint32 durationBytes = (uint32)duration * kBytesPerFrame;
        if (startByte + durationBytes < endByte)
            endByte = startByte + durationBytes;
    }
    if (endByte <= startByte) {
        warning("[audiocd] track %d has empty byte window", track);
        delete bin;
        return false;
    }
    uint32 windowBytes = (endByte - startByte) & ~3u;

    Common::SeekableReadStream *hwWindow =
        new Common::SafeSeekableSubReadStream(bin, startByte,
                                              startByte + windowBytes,
                                              DisposeAfterUse::YES);
    OpenFPGA::HardwareCDDARing *hw =
        new OpenFPGA::HardwareCDDARing(hwWindow, windowBytes, numLoops,
                                       _mixer, soundType, _cd.volume,
                                       _cd.balance);
    if (hw->start()) {
        _cd.playing  = true;
        _cd.track    = track;
        _cd.start    = startFrame;
        _cd.duration = duration;
        _cd.numLoops = numLoops;
        _emulating   = true;
        _activeHW = hw;
        _paused = false;

        debug(1, "[audiocd] HW mixer track=%d startFrame=%d duration=%d bytes=%u",
              track, startFrame, duration, windowBytes);
        return true;
    }

    warning("[audiocd] HW mixer path failed; falling back to ScummVM stream");
    delete hw; /* also deletes hwWindow/bin */

    bin = openNamedStream(e->binFile, _cuePath);
    if (!bin) {
        warning("[audiocd] failed to reopen '%s' for fallback track %d",
                e->binFile.c_str(), track);
        return false;
    }

    Common::SeekableReadStream *syncWindow =
        new Common::SafeSeekableSubReadStream(bin, startByte,
                                              startByte + windowBytes,
                                              DisposeAfterUse::YES);

    int asyncSlot = findDataSlot(e->binFile);
    OpenFPGA::BufferedCDDAStream *stream =
        new OpenFPGA::BufferedCDDAStream(syncWindow, windowBytes, numLoops,
                                        asyncSlot, startByte);
    if (!stream->prime()) {
        warning("[audiocd] failed to prime track %d", track);
        delete stream;
        return false;
    }

    /* Update cd status the way DefaultAudioCDManager does so isPlaying()
     * etc. report correctly. */
    _cd.playing  = true;
    _cd.track    = track;
    _cd.start    = startFrame;
    _cd.duration = duration;
    _cd.numLoops = numLoops;
    _emulating   = true;
    _activeStream = stream;
    _paused = false;

    _mixer->playStream(soundType, &_handle, stream, -1, _cd.volume,
                       _cd.balance, DisposeAfterUse::NO);

    debug(1, "[audiocd] play track=%d startFrame=%d duration=%d bytes=%u %s",
          track, startFrame, duration, windowBytes,
          asyncSlot >= 0 ? "async" : "sync-fallback");
    return true;
}

void OpenFPGAAudioCDManager::stop() {
    if (_activeHW) {
        delete _activeHW;
        _activeHW = nullptr;
    } else if (_emulating) {
        _mixer->stopHandle(_handle);
    }
    delete _activeStream;
    _activeStream = nullptr;
    _emulating = false;
    _cd.playing = false;
    _paused = false;
}

bool OpenFPGAAudioCDManager::isPlaying() const {
    if (_activeHW)
        return _activeHW->isActive();
    return DefaultAudioCDManager::isPlaying();
}

void OpenFPGAAudioCDManager::setVolume(byte volume) {
    _cd.volume = volume;
    if (_activeHW)
        _activeHW->setVolume(_cd.volume, _cd.balance);
    else if (_emulating && DefaultAudioCDManager::isPlaying())
        _mixer->setChannelVolume(_handle, _cd.volume);
}

void OpenFPGAAudioCDManager::setBalance(int8 balance) {
    _cd.balance = balance;
    if (_activeHW)
        _activeHW->setVolume(_cd.volume, _cd.balance);
    else if (_emulating && DefaultAudioCDManager::isPlaying())
        _mixer->setChannelBalance(_handle, _cd.balance);
}

void OpenFPGAAudioCDManager::update() {
    if (_paused)
        return;
    if (_activeHW) {
        _activeHW->pump(true);
        if (!_activeHW->isActive()) {
            delete _activeHW;
            _activeHW = nullptr;
            _emulating = false;
            _cd.playing = false;
        }
        return;
    }
    pump();
    if (_emulating && !_mixer->isSoundHandleActive(_handle)) {
        delete _activeStream;
        _activeStream = nullptr;
        _emulating = false;
        _cd.playing = false;
    }
}

void OpenFPGAAudioCDManager::setPaused(bool paused) {
    if (_activeHW)
        _activeHW->setPaused(paused);
    else if (paused && _activeStream)
        _activeStream->quiesce();
    _paused = paused;
}

bool OpenFPGAAudioCDManager::existExtractedCDAudioFiles(uint track) {
    if (_cueLoaded && findTrack((int)track + _trackOffset))
        return true;
    if (_cueLoaded && _trackOffset == 0 && _implicitTrackOffset != 0 &&
        findTrack((int)track + _implicitTrackOffset))
        return true;
    return DefaultAudioCDManager::existExtractedCDAudioFiles(track);
}

void OpenFPGAAudioCDManager::pump(bool allowBatch) {
    if (_paused)
        return;
    if (_activeHW)
        _activeHW->pump(allowBatch);
    else if (_activeStream)
        _activeStream->pump(false);
}

void OpenFPGAAudioCDManager::quiesce() {
    if (_activeHW)
        _activeHW->quiesce();
    else if (_activeStream)
        _activeStream->quiesce();
}

extern "C" void openfpga_audiocd_pump(void) {
    if (g_activeCDManager)
        g_activeCDManager->pump(true);
}

/* Light pump for call sites that sit on the latency-critical path (mid-present,
 * between zip read chunks): keeps the HW cursor/volume serviced but defers the
 * batched blocking refill to a full pump unless the buffer hits the emergency
 * floor. */
extern "C" void openfpga_audiocd_pump_light(void) {
    if (g_activeCDManager)
        g_activeCDManager->pump(false);
}

extern "C" void openfpga_audiocd_pause(bool paused) {
    if (g_activeCDManager)
        g_activeCDManager->setPaused(paused);
}

extern "C" void openfpga_audiocd_quiesce(void) {
    if (g_activeCDManager)
        g_activeCDManager->quiesce();
}
