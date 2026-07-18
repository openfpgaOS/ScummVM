/*
 * openfpga_save.cpp -- SaveFileManager backed by APF save slots.
 *
 * main.cpp maps slot numbers to the actual launcher-bound .sav filenames
 * at startup.  Each file opens as normal POSIX I/O via the SDK; fclose()
 * on a write-opened nonvolatile slot commits the byte count to SD card.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_save.h"

#include "common/debug.h"
#include "common/memstream.h"
#include "common/stream.h"
#include "common/str.h"
#include "common/textconsole.h"

extern "C" {
#include <of_file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

extern "C" void openfpga_audiocd_quiesce(void);
/* Light audio pump (PCM mixer only, re-entry safe via g_pumpBusy) -- keeps
 * the ring fed across the blocking bridge reads below.  Same pattern as
 * SlotFileStream::read in main.cpp. */
extern void openfpga_mixer_pump_only(void);

namespace {

char g_savePaths[OPENFPGA_MAX_SAVES][256];
byte g_saveWriteBuffer[SAVE_DATA_MAX];
bool g_saveWriteInUse = false;

FILE *openSlot(int slot, const char *mode) {
    if (slot < 0 || slot >= OPENFPGA_MAX_SAVES || !g_savePaths[slot][0])
        return nullptr;
    return fopen(g_savePaths[slot], mode);
}

bool writeExact(FILE *f, const void *data, uint32 bytes) {
    const byte *src = (const byte *)data;
    while (bytes) {
        const uint32 chunk = bytes > 4096 ? 4096 : bytes;
        if (fwrite(src, 1, chunk, f) != chunk)
            return false;
        src += chunk;
        bytes -= chunk;
    }
    return true;
}

/* Wildcard match for the patterns ScummVM passes to listSavefiles, matching
 * Common::matchString's convention: '*' = 0+ chars, '?' = any one char, and
 * '#' = one DIGIT.  The '#' case is essential: SCI's native save/load lists
 * with getSavegamePattern() = "<target>.###" (three digit wildcards), so
 * without it "lsl3.001" never matches "lsl3.###" and the load list is empty. */
bool patternMatch(const char *name, const char *pat) {
    while (*pat && *name) {
        if (*pat == '*') return true;
        if (*pat == '?') { pat++; name++; continue; }
        if (*pat == '#') {
            if (*name < '0' || *name > '9') return false;
            pat++; name++; continue;
        }
        if (*pat != *name) return false;
        pat++; name++;
    }
    if (*pat == '*') return true;
    return *pat == '\0' && *name == '\0';
}

/* Fixed-buffer write stream matching the Doom port's save model:
 * accumulate one complete save in RAM, then write it to the registered
 * nonvolatile slot name with stdio and use fclose() as the commit result.
 * Avoid MemoryWriteStreamDynamic here; reallocating near the slot limit is
 * exactly the kind of heap pressure that has caused Pocket save crashes. */
class SlotOutStream : public Common::SeekableWriteStream {
public:
    SlotOutStream(int slot, const Common::String &name)
        : _slot(slot), _name(name), _pos(0), _size(0),
          _flushed(false), _err(false) {
        g_saveWriteInUse = true;
        memset(g_saveWriteBuffer, 0, sizeof(g_saveWriteBuffer));
    }

    ~SlotOutStream() override {
        flushToSlot();
        g_saveWriteInUse = false;
    }

    uint32 write(const void *dataPtr, uint32 dataSize) override {
        if (_flushed) {
            _err = true;
            return 0;
        }

        if (_pos > SAVE_DATA_MAX) {
            _err = true;
            return 0;
        }

        uint32 writable = SAVE_DATA_MAX - _pos;
        uint32 toWrite = dataSize > writable ? writable : dataSize;
        if (toWrite) {
            memcpy(g_saveWriteBuffer + _pos, dataPtr, toWrite);
            _pos += toWrite;
            if (_pos > _size)
                _size = _pos;
        }
        if (toWrite != dataSize) {
            if (!_err)
                warning("[save] '%s' exceeds the %u-byte slot -- save aborted",
                        _name.c_str(), (uint32)SAVE_DATA_MAX);
            _err = true;
        }
        return toWrite;
    }

    int64 pos() const override { return _pos; }
    int64 size() const override { return _size; }

    bool seek(int64 offset, int whence = SEEK_SET) override {
        int64 target = 0;
        switch (whence) {
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = (int64)_pos + offset;
            break;
        case SEEK_END:
            target = (int64)_size + offset;
            break;
        default:
            _err = true;
            return false;
        }

        if (target < 0 || target > (int64)SAVE_DATA_MAX) {
            _err = true;
            return false;
        }
        if ((uint32)target > _size) {
            memset(g_saveWriteBuffer + _size, 0, (uint32)target - _size);
            _size = (uint32)target;
        }
        _pos = (uint32)target;
        return true;
    }

    void finalize() override {
        flushToSlot();
    }

    bool flush() override {
        return !err();
    }

    bool err() const override {
        return _err;
    }

    void clearErr() override {
        _err = false;
    }

private:
    void flushToSlot() {
        if (_flushed) return;
        _flushed = true;

        /* Never commit a bad (e.g. overflowed/truncated) save: a truncated
         * payload still parses as a valid save and would silently replace a
         * good one, then load back with its tail sections zeroed.  Leaving
         * the slot untouched keeps the previous save intact and lets the
         * engine report "Game NOT saved". */
        if (_err) {
            warning("[save] '%s' not committed (write error/overflow); "
                    "previous slot contents preserved", _name.c_str());
            return;
        }

        uint32 dataLen = _size;
        uint32 totalLen = dataLen + (uint32)sizeof(SaveSlotHeader);
        if (dataLen > SAVE_DATA_MAX) {
            warning("[save] '%s' too large: payload=%u max=%u",
                    _name.c_str(), dataLen, (uint32)SAVE_DATA_MAX);
            _err = true;
            return;
        }

        /* Payload vs the 256 KB slot cap (saves are raw -- zlib is stubbed);
         * enable debuglevel 1 to watch for saves creeping toward the cap. */
        debug(1, "[save] slot=%d name='%s' payload=%u/%u total=%u",
              _slot, _name.c_str(), dataLen, (uint32)SAVE_DATA_MAX, totalLen);

        openfpga_audiocd_quiesce();

        FILE *f = openSlot(_slot, "wb");
        if (!f) { _err = true; return; }

        /* Two-phase, magic-LAST commit.  The slot commit is a long blocking
         * window (256 KB over the bridge + the nonvolatile fclose); a
         * power-off/sleep in that window used to leave a VALID header over
         * incomplete payload -- readHeader accepted it and the engine got
         * garbage (the "WRONG SAVE TYPE" / assert-on-load corruption
         * reports).  Now the header goes out with magic=0 first, payload
         * second, and the real magic is written LAST: an interrupted commit
         * fails the magic check and reads as an empty slot instead of a
         * plausible save.  No format change -- completed saves are
         * byte-identical to before. */
        SaveSlotHeader hdr;
        hdr.magic = 0;
        memset(hdr.filename, 0, SAVE_NAME_MAX);
        strncpy(hdr.filename, _name.c_str(), SAVE_NAME_MAX - 1);
        hdr.dataLen = dataLen;

        if (!writeExact(f, &hdr, sizeof(hdr)) ||
            !writeExact(f, g_saveWriteBuffer, dataLen)) {
            _err = true;
        }
        if (!_err) {
            hdr.magic = SAVE_MAGIC;
            if (fseek(f, 0, SEEK_SET) != 0 ||
                !writeExact(f, &hdr, sizeof(hdr)))
                _err = true;
        }
        if (fclose(f) != 0)
            _err = true;
    }

    int             _slot;
    Common::String  _name;
    uint32          _pos;
    uint32          _size;
    bool            _flushed;
    bool            _err;
};

} // namespace

void openfpga_set_save_path(int slot, const char *path) {
    if (slot < 0 || slot >= OPENFPGA_MAX_SAVES)
        return;
    if (!path) {
        g_savePaths[slot][0] = '\0';
        return;
    }
    snprintf(g_savePaths[slot], sizeof(g_savePaths[slot]), "%s", path);
    of_file_slot_register(10 + (uint32)slot, g_savePaths[slot]);
}

/* ── Slot helpers ─────────────────────────────────────────────────── */

bool OpenFPGASaveFileManager::readHeader(int slot, SaveSlotHeader *hdr) {
    FILE *f = openSlot(slot, "rb");
    if (!f) return false;
    size_t n = fread(hdr, 1, sizeof(SaveSlotHeader), f);
    fclose(f);
    /* The restore dialog and findSlotByName/findFreeSlot scan all 9 slots
     * back to back (fopen+fread+fclose each) -- pump once per slot so slow
     * bridge round-trips can't accumulate into a dropout while menu music
     * is playing. */
    openfpga_mixer_pump_only();
    if (n != sizeof(SaveSlotHeader)) return false;
    if (hdr->magic != SAVE_MAGIC) return false;
    if (hdr->dataLen == 0 || hdr->dataLen > SAVE_DATA_MAX) return false;
    hdr->filename[SAVE_NAME_MAX - 1] = '\0';
    return hdr->filename[0] != '\0';
}

int OpenFPGASaveFileManager::findSlotByName(const char *name) {
    SaveSlotHeader hdr;
    for (int i = 0; i < OPENFPGA_MAX_SAVES; i++) {
        if (readHeader(i, &hdr) && strncmp(hdr.filename, name, SAVE_NAME_MAX) == 0)
            return i;
    }
    return -1;
}

int OpenFPGASaveFileManager::findFreeSlot() {
    SaveSlotHeader hdr;
    for (int i = 0; i < OPENFPGA_MAX_SAVES; i++) {
        if (!g_savePaths[i][0])
            continue;
        if (!readHeader(i, &hdr)) return i;
    }
    return -1;
}

/* ── SaveFileManager interface ────────────────────────────────────── */

Common::InSaveFile *OpenFPGASaveFileManager::openRawFile(const Common::String &name) {
    return openForLoading(name);
}

Common::InSaveFile *OpenFPGASaveFileManager::openForLoading(const Common::String &name) {
    int slot = findSlotByName(name.c_str());
    if (slot < 0) return nullptr;

    FILE *f = openSlot(slot, "rb");
    if (!f) return nullptr;

    SaveSlotHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr.magic != SAVE_MAGIC || hdr.dataLen == 0 ||
        hdr.dataLen > SAVE_DATA_MAX ||
        strncmp(hdr.filename, name.c_str(), SAVE_NAME_MAX) != 0) {
        fclose(f);
        return nullptr;
    }

    /* A payload that exactly fills the slot almost certainly hit the write
     * clamp in an older build: the tail was never written, and feeding the
     * zero-tailed payload to the engine's serializer ends in an engine
     * assert (SCUMM saveload resource loop) and a dead session.  REFUSE the
     * load -- the engine reports a clean per-slot failure instead, and the
     * user's other slots stay reachable.  (A legitimate save of exactly
     * SAVE_DATA_MAX bytes is indistinguishable from a truncated one and
     * pays the price; the odds are negligible.) */
    if (hdr.dataLen >= SAVE_DATA_MAX) {
        warning("[save] '%s' fills the whole slot -- truncated by an older "
                "build's write clamp; refusing to load it", name.c_str());
        fclose(f);
        return nullptr;
    }

    byte *data = (byte *)malloc(hdr.dataLen);
    if (!data) { fclose(f); return nullptr; }

    /* Chunk the payload read (up to 256 KB) and pump the mixer between
     * chunks: restore runs while menu music is playing, and one unpumped
     * bridge read of that size is an audible dropout. */
    uint32 got = 0;
    bool readOk = true;
    while (got < hdr.dataLen) {
        const uint32 chunk = (hdr.dataLen - got > 8192) ? 8192 : hdr.dataLen - got;
        if (fread(data + got, 1, chunk, f) != chunk) {
            readOk = false;
            break;
        }
        got += chunk;
        openfpga_mixer_pump_only();
    }
    if (!readOk) {
        free(data);
        fclose(f);
        return nullptr;
    }
    fclose(f);

    /* MemoryReadStream takes ownership of `data`; it will free() it. */
    return new Common::MemoryReadStream(data, hdr.dataLen, DisposeAfterUse::YES);
}

Common::OutSaveFile *OpenFPGASaveFileManager::openForSaving(const Common::String &name, bool compress) {
    if (g_saveWriteInUse) {
        setError(Common::kWritingFailed, "A save is already in progress");
        return nullptr;
    }
    int slot = findSlotByName(name.c_str());
    if (slot < 0) slot = findFreeSlot();
    if (slot < 0) {
        setError(Common::kWritingFailed, "All save slots are full or unavailable");
        return nullptr;
    }
    /* zlib is stubbed in this build, so `compress` is a no-op even via
     * wrapCompressedWriteStream — skip it entirely and write raw. */
    (void)compress;
    return new Common::OutSaveFile(new SlotOutStream(slot, name));
}

bool OpenFPGASaveFileManager::removeSavefile(const Common::String &name) {
    int slot = findSlotByName(name.c_str());
    if (slot < 0) return false;
    /* Open for write, close immediately: produces a 0-byte slot that
     * fails the magic check on next read. */
    FILE *f = openSlot(slot, "wb");
    if (!f) return false;
    return fclose(f) == 0;
}

Common::StringArray OpenFPGASaveFileManager::listSavefiles(const Common::String &pattern) {
    Common::StringArray out;
    SaveSlotHeader hdr;
    for (int i = 0; i < OPENFPGA_MAX_SAVES; i++) {
        if (!readHeader(i, &hdr)) continue;
        if (patternMatch(hdr.filename, pattern.c_str()))
            out.push_back(Common::String(hdr.filename));
    }
    return out;
}

void OpenFPGASaveFileManager::updateSavefilesList(Common::StringArray &lockedFiles) {
    (void)lockedFiles;
}

bool OpenFPGASaveFileManager::exists(const Common::String &name) {
    return findSlotByName(name.c_str()) >= 0;
}
