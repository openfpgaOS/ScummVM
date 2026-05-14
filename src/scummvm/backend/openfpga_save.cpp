/*
 * openfpga_save.cpp -- SaveFileManager backed by APF save slots.
 *
 * Slot path is "save:N" (N=0..9).  Each slot opens up as a normal POSIX
 * file via the SDK; fclose() on a write-opened slot auto-flushes the
 * actual byte count back to SD card.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_save.h"

#include "common/memstream.h"
#include "common/stream.h"
#include "common/str.h"

extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

namespace {

FILE *openSlot(int slot, const char *mode) {
    char path[16];
    snprintf(path, sizeof(path), "save:%d", slot);
    return fopen(path, mode);
}

/* Trailing-'*' wildcard match.  Matches the simple patterns ScummVM
 * uses for listSavefiles ("monkey.s##" gets translated by the engine
 * to specific names, "scummvm-*" style patterns also occur). */
bool patternMatch(const char *name, const char *pat) {
    while (*pat && *name) {
        if (*pat == '*') return true;
        if (*pat == '?') { pat++; name++; continue; }
        if (*pat != *name) return false;
        pat++; name++;
    }
    if (*pat == '*') return true;
    return *pat == '\0' && *name == '\0';
}

/* In-memory write stream that flushes its accumulated bytes to a slot
 * on finalize() / destruction.  ScummVM expects writes to succeed
 * immediately, and we don't want to interleave seeks with slot I/O. */
class SlotOutStream : public Common::MemoryWriteStreamDynamic {
public:
    SlotOutStream(int slot, const Common::String &name)
        : Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES),
          _slot(slot), _name(name), _flushed(false), _err(false) {}

    ~SlotOutStream() override { flushToSlot(); }

    bool flush() override {
        flushToSlot();
        return !_err;
    }

    bool err() const override {
        return _err || Common::MemoryWriteStreamDynamic::err();
    }

private:
    void flushToSlot() {
        if (_flushed) return;
        _flushed = true;

        uint32 dataLen = size();
        if (dataLen > SAVE_DATA_MAX) { _err = true; return; }

        FILE *f = openSlot(_slot, "wb");
        if (!f) { _err = true; return; }

        SaveSlotHeader hdr;
        hdr.magic = SAVE_MAGIC;
        memset(hdr.filename, 0, SAVE_NAME_MAX);
        strncpy(hdr.filename, _name.c_str(), SAVE_NAME_MAX - 1);
        hdr.dataLen = dataLen;

        if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
            (dataLen && fwrite(getData(), 1, dataLen, f) != dataLen)) {
            _err = true;
        }
        fclose(f);
    }

    int             _slot;
    Common::String  _name;
    bool            _flushed;
    bool            _err;
};

} // namespace

/* ── Slot helpers ─────────────────────────────────────────────────── */

bool OpenFPGASaveFileManager::readHeader(int slot, SaveSlotHeader *hdr) {
    FILE *f = openSlot(slot, "rb");
    if (!f) return false;
    size_t n = fread(hdr, 1, sizeof(SaveSlotHeader), f);
    fclose(f);
    if (n != sizeof(SaveSlotHeader)) return false;
    return hdr->magic == SAVE_MAGIC;
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
        hdr.magic != SAVE_MAGIC || hdr.dataLen > SAVE_DATA_MAX) {
        fclose(f);
        return nullptr;
    }

    byte *data = (byte *)malloc(hdr.dataLen);
    if (!data) { fclose(f); return nullptr; }

    if (hdr.dataLen && fread(data, 1, hdr.dataLen, f) != hdr.dataLen) {
        free(data);
        fclose(f);
        return nullptr;
    }
    fclose(f);

    /* MemoryReadStream takes ownership of `data`; it will free() it. */
    return new Common::MemoryReadStream(data, hdr.dataLen, DisposeAfterUse::YES);
}

Common::OutSaveFile *OpenFPGASaveFileManager::openForSaving(const Common::String &name, bool compress) {
    int slot = findSlotByName(name.c_str());
    if (slot < 0) slot = findFreeSlot();
    if (slot < 0) {
        setError(Common::kWritingFailed, "All save slots are full");
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
    fclose(f);
    return true;
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
