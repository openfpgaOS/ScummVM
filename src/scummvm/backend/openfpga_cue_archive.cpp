/*
 * openfpga_cue_archive.cpp -- ISO 9660 over MODE1/2352 raw bin.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_cue_archive.h"

#include "common/debug.h"
#include "common/file.h"
#include "common/memstream.h"
#include "common/substream.h"
#include "common/textconsole.h"
#include "common/util.h"

extern "C" {
#include <stdio.h>
#include <string.h>
#include <strings.h>
}

namespace OpenFPGA {

namespace {

constexpr uint32 kRawSector    = 2352;
constexpr uint32 kCookedSector = 2048;
constexpr uint32 kSyncSize     = 16;       /* 12 sync bytes + 4 header bytes */

struct ParsedDataTrack {
    Common::String fileName;
    int index0;
    int index1;
    bool valid;

    ParsedDataTrack() : index0(-1), index1(-1), valid(false) {}
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
    Common::ArchiveMemberPtr member = SearchMan.getMember(Common::Path(name));
    if (member)
        return member->createReadStream();

    FILE *f = fopen(name.c_str(), "rb");
    if (f)
        return new StdioSeekableReadStream(f);

    return nullptr;
}

Common::SeekableReadStream *openNamedStream(const Common::String &name,
                                            const Common::String &cuePath) {
    if (name.empty())
        return nullptr;

    Common::SeekableReadStream *stream = openNamedStreamOnce(name);
    if (stream)
        return stream;

    if (!hasDirectoryComponent(name.c_str())) {
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

bool parseCueDataTrack(char *cueText, ParsedDataTrack &out) {
    char currentFile[256] = "";
    bool pendingData = false;
    ParsedDataTrack pending;

    auto commitPending = [&]() {
        if (!out.valid && pendingData && !pending.fileName.empty()) {
            out = pending;
            out.valid = true;
        }
    };

    char *line = cueText;
    while (line && *line && !out.valid) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        trimLine(line);
        char *p = skipSpaces(line);

        if (startsWithI(p, "FILE")) {
            parseCueFileName(p, currentFile, sizeof(currentFile));
        } else if (startsWithI(p, "TRACK")) {
            commitPending();
            pendingData = false;
            pending = ParsedDataTrack();

            char type[32];
            int number = 0;
            if (parseCueTrack(p, number, type, sizeof(type)) &&
                (!strcasecmp(type, "MODE1/2352") ||
                 !strcasecmp(type, "MODE1/RAW"))) {
                pendingData = true;
                pending.fileName = currentFile;
            }
        } else if (startsWithI(p, "INDEX")) {
            int indexNo = -1;
            int frame = -1;
            if (pendingData && parseCueIndex(p, indexNo, frame)) {
                if (indexNo == 0)
                    pending.index0 = frame;
                else if (indexNo == 1)
                    pending.index1 = frame;
            }
        }

        line = next;
    }
    commitPending();
    return out.valid;
}

} // namespace

/* ───────────────────────────────────────────────────────────────────
 * Mode1RawStream — wraps a raw .bin (MODE1/2352) as a cooked 2048/sec.
 * Reads pass through 1:1 except the per-sector 16-byte sync header is
 * skipped.  Seek positions are in COOKED bytes; we translate to raw
 * by sector index.
 * ─────────────────────────────────────────────────────────────────── */

class Mode1RawStream : public Common::SeekableReadStream {
public:
    Mode1RawStream(Common::SeekableReadStream *raw, uint32 dataStartByte,
                   uint32 dataSectorCount, DisposeAfterUse::Flag dispose)
        : _raw(raw), _dispose(dispose),
          _dataStart(dataStartByte),
          _sectorCount(dataSectorCount),
          _cookedSize((int64)dataSectorCount * kCookedSector),
          _pos(0), _err(false), _eos(false) {}

    ~Mode1RawStream() override {
        if (_dispose == DisposeAfterUse::YES) delete _raw;
    }

    bool eos()  const override { return _eos; }
    bool err()  const override { return _err || _raw->err(); }
    void clearErr() override   { _err = false; _eos = false; _raw->clearErr(); }
    int64 pos()  const override { return _pos; }
    int64 size() const override { return _cookedSize; }

    bool seek(int64 offset, int whence = SEEK_SET) override {
        int64 newPos;
        switch (whence) {
        case SEEK_SET: newPos = offset; break;
        case SEEK_CUR: newPos = _pos + offset; break;
        case SEEK_END: newPos = _cookedSize + offset; break;
        default: return false;
        }
        if (newPos < 0 || newPos > _cookedSize) return false;
        _pos = newPos;
        _eos = false;
        return true;
    }

    uint32 read(void *dst, uint32 cnt) override {
        uint8 *d = (uint8 *)dst;
        uint32 total = 0;
        if (_pos >= _cookedSize) { _eos = true; return 0; }
        if ((int64)cnt > _cookedSize - _pos)
            cnt = (uint32)(_cookedSize - _pos);

        while (cnt > 0) {
            uint32 secIdx   = (uint32)(_pos / kCookedSector);
            uint32 inSecOff = (uint32)(_pos % kCookedSector);
            uint32 chunk    = kCookedSector - inSecOff;
            if (chunk > cnt) chunk = cnt;

            uint32 rawOff = _dataStart + secIdx * kRawSector + kSyncSize + inSecOff;
            if (!_raw->seek(rawOff, SEEK_SET)) { _err = true; break; }
            uint32 got = _raw->read(d, chunk);
            if (got == 0) { _err = _raw->err(); _eos = !_err; break; }

            d     += got;
            total += got;
            _pos  += got;
            cnt   -= got;
        }
        return total;
    }

private:
    Common::SeekableReadStream *_raw;
    DisposeAfterUse::Flag       _dispose;
    uint32 _dataStart;       /* byte offset of Track 01 in raw stream */
    uint32 _sectorCount;
    int64  _cookedSize;
    int64  _pos;
    bool   _err;
    bool   _eos;
};

/* ───────────────────────────────────────────────────────────────────
 * CueArchive
 * ─────────────────────────────────────────────────────────────────── */

CueArchive::CueArchive() : _cooked(nullptr) {}

CueArchive::~CueArchive() { delete _cooked; }

CueArchive *CueArchive::createISO(const Common::String &isoPath) {
    Common::SeekableReadStream *isoStream = openNamedStreamOnce(isoPath);
    if (!isoStream) {
        warning("[iso-archive] cannot open iso '%s'", isoPath.c_str());
        return nullptr;
    }

    CueArchive *a = new CueArchive();
    a->_cooked = isoStream;
    if (!a->parseISO9660()) {
        warning("[iso-archive] ISO9660 parse failed for '%s'",
                isoPath.c_str());
        delete a;
        return nullptr;
    }

    debug(1, "[iso-archive] mounted '%s' (%u files)",
          isoPath.c_str(), (uint32)a->_files.size());
    return a;
}

CueArchive *CueArchive::create(const Common::String &cuePath) {
    /* Open the cue via SearchMan's archive members rather than
     * Common::File::open, so we don't get tangled in path encoding
     * for filenames with spaces / parens.  ArchiveMember::createReadStream
     * goes straight to the zip entry. */
    Common::SeekableReadStream *cueStream = openNamedStreamOnce(cuePath);
    if (!cueStream) {
        /* Fallback: any .cue file SearchMan can find. */
        Common::ArchiveMemberList cues;
        SearchMan.listMatchingMembers(cues, "*.cue");
        debug(1, "[cue-archive] '%s' not found directly; %u .cue candidates in SearchMan:",
              cuePath.c_str(), (uint32)cues.size());
        for (auto it = cues.begin(); it != cues.end(); ++it)
            debug(1, "[cue-archive]   '%s'", (*it)->getName().c_str());
        if (!cues.empty()) {
            cueStream = cues.front()->createReadStream();
            debug(1, "[cue-archive] using '%s' as fallback",
                  cues.front()->getName().c_str());
        }
    }

    if (!cueStream) {
        warning("[cue-archive] cannot open cue '%s'", cuePath.c_str());
        return nullptr;
    }

    uint32 cueSize = (uint32)cueStream->size();
    char *cueText = new char[cueSize + 1];
    uint32 cueRead = cueStream->read(cueText, cueSize);
    cueText[cueRead] = '\0';
    delete cueStream;

    ParsedDataTrack dataTrack;
    bool haveDataTrack = parseCueDataTrack(cueText, dataTrack);
    delete[] cueText;

    if (!haveDataTrack) {
        warning("[cue-archive] no MODE1/2352 data track in cue");
        return nullptr;
    }

    /* Same trick for the bin file -- go via SearchMan member to avoid
     * Common::File path-encoding pitfalls. */
    Common::SeekableReadStream *binStream = openNamedStream(dataTrack.fileName, cuePath);
    if (!binStream) {
        /* Fallback: any .bin SearchMan sees. */
        Common::ArchiveMemberList bins;
        SearchMan.listMatchingMembers(bins, "*.bin");
        debug(1, "[cue-archive] track 01 .bin '%s' not found; %u .bin candidates:",
              dataTrack.fileName.c_str(), (uint32)bins.size());
        for (auto it = bins.begin(); it != bins.end(); ++it)
            debug(1, "[cue-archive]   '%s'", (*it)->getName().c_str());
        /* Find the first .bin whose name starts with the same
         * stem as the cue requested (catch "Track 01" via substring). */
        for (auto it = bins.begin(); it != bins.end(); ++it) {
            const Common::String &n = (*it)->getName();
            if (n.contains("01.bin") || n.contains("01) ") ||
                n.contains("(Track 01)")) {
                binStream = (*it)->createReadStream();
                debug(1, "[cue-archive] using '%s' as data .bin", n.c_str());
                break;
            }
        }
    }
    if (!binStream) {
        warning("[cue-archive] cannot open data .bin '%s'",
                dataTrack.fileName.c_str());
        return nullptr;
    }

    /* Find the INDEX 01 frame for this track within its .bin file.
     * For per-track .bin layout most rippers use, INDEX 01 is at 0 for
     * the data track (no pregap on track 01).  Fall back to INDEX 00 or
     * 0 if missing. */
    int dataFrame = 0;
    if (dataTrack.index1 >= 0)
        dataFrame = dataTrack.index1;
    else if (dataTrack.index0 >= 0)
        dataFrame = dataTrack.index0;

    uint32 dataStartByte = (uint32)dataFrame * kRawSector;
    uint32 binSize       = (uint32)binStream->size();
    if (dataStartByte > binSize) {
        warning("[cue-archive] data start %u beyond bin size %u",
                dataStartByte, binSize);
        delete binStream;
        return nullptr;
    }
    uint32 sectorCount = (binSize - dataStartByte) / kRawSector;

    CueArchive *a = new CueArchive();
    a->_cooked = new Mode1RawStream(binStream, dataStartByte, sectorCount,
                                    DisposeAfterUse::YES);

    if (!a->parseISO9660()) {
        warning("[cue-archive] ISO9660 parse failed");
        delete a;
        return nullptr;
    }
    debug(1, "[cue-archive] mounted '%s' (%u sectors, %u files)",
          dataTrack.fileName.c_str(), sectorCount,
          (uint32)a->_files.size());
    return a;
}

/* ───────────────────────────────────────────────────────────────────
 * ISO 9660 parsing
 * Primary Volume Descriptor at LBA 16, offset 156 holds the root
 * directory record (34 bytes).  Each record:
 *   +0  : record length (0 = end of sector, skip to next)
 *   +2  : extent LBA, LSB form, 4 bytes
 *   +10 : extent size, LSB form, 4 bytes
 *   +25 : file flags (bit 1 = directory)
 *   +32 : name length
 *   +33 : name bytes
 * ─────────────────────────────────────────────────────────────────── */

namespace {

uint32 readLE32(const byte *p) {
    return ((uint32)p[0]) | ((uint32)p[1] << 8) |
           ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

Common::String stripVersion(const Common::String &n) {
    /* ISO names end in ";<ver>" — drop it.  Also drop trailing '.' for
     * extensionless names (some games store "MONKEY.000;1" but a few
     * tools generate just "FILE.;1"). */
    int sc = n.findLastOf(';');
    Common::String out = (sc > 0) ? Common::String(n.c_str(), sc) : n;
    while (out.size() > 0 && out.lastChar() == '.')
        out.deleteLastChar();
    return out;
}

} // namespace

bool CueArchive::parseISO9660() {
    /* Read Primary Volume Descriptor at LBA 16 */
    if (!_cooked->seek((int64)16 * kCookedSector, SEEK_SET)) return false;
    byte pvd[kCookedSector];
    if (_cooked->read(pvd, kCookedSector) != kCookedSector) return false;

    /* PVD signature: type=1, id="CD001", version=1 at offsets 0,1,6 */
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        warning("[cue-archive] no PVD at LBA 16 (got type=%u id='%c%c%c%c%c')",
                pvd[0], pvd[1], pvd[2], pvd[3], pvd[4], pvd[5]);
        return false;
    }

    /* Root directory record at offset 156, 34 bytes */
    const byte *root = pvd + 156;
    uint32 rootLBA  = readLE32(root + 2);
    uint32 rootSize = readLE32(root + 10);
    return parseDirectory(rootLBA, rootSize, Common::String());
}

bool CueArchive::parseDirectory(uint32 lba, uint32 size,
                                 const Common::String &prefix) {
    /* Read the whole extent into RAM — directories on game discs are
     * always tiny (a few KB). */
    byte *buf = new byte[size];
    if (!_cooked->seek((int64)lba * kCookedSector, SEEK_SET) ||
        _cooked->read(buf, size) != size) {
        delete[] buf;
        return false;
    }

    Common::Array<Entry> subdirs;
    Common::Array<Common::String> subdirNames;

    uint32 off = 0;
    while (off < size) {
        byte recLen = buf[off];
        if (recLen == 0) {
            /* Records do not span sectors; if zero, skip to next sector. */
            uint32 next = ((off / kCookedSector) + 1) * kCookedSector;
            if (next >= size) break;
            off = next;
            continue;
        }
        if (off + recLen > size) break;
        if (recLen < 33) { off += recLen; continue; }

        const byte *r = buf + off;
        uint32 entLBA  = readLE32(r + 2);
        uint32 entSize = readLE32(r + 10);
        byte   flags   = r[25];
        byte   nameLen = r[32];
        const char *name = (const char *)(r + 33);

        /* ISO records are self-contained and must not cross a sector.
         * Some images include padding and non-file records; skip anything
         * whose declared name would run past the record itself. */
        uint32 sectorEnd = ((off / kCookedSector) + 1) * kCookedSector;
        if (sectorEnd > size)
            sectorEnd = size;
        if (off + recLen > sectorEnd || 33u + nameLen > recLen) {
            off += recLen;
            continue;
        }

        /* Skip "." (name = single 0x00) and ".." (name = single 0x01). */
        if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) {
            off += recLen;
            continue;
        }

        Common::String ent(name, nameLen);
        Common::String full = prefix.empty()
                              ? ent
                              : prefix + "/" + ent;

        if (flags & 0x02) {
            /* Directory — recurse later (avoid using `_cooked` while
             * we still have a sibling iterator open on the same stream
             * via seek/read above; collect first, recurse after we
             * release `buf`). */
            Entry sub = { entLBA, entSize };
            subdirs.push_back(sub);
            subdirNames.push_back(full);
        } else {
            Common::String key = stripVersion(ent);
            /* Store under bare name (engine looks up files by basename
             * via SearchMan) AND under full path for completeness. */
            Entry e = { entLBA, entSize };
            _files[key]  = e;
            Common::String fullKey = stripVersion(full);
            if (fullKey != key) _files[fullKey] = e;
        }

        off += recLen;
    }

    delete[] buf;

    for (uint i = 0; i < subdirs.size(); ++i)
        parseDirectory(subdirs[i].lba, subdirs[i].size, subdirNames[i]);

    return true;
}

/* ───────────────────────────────────────────────────────────────────
 * Common::Archive overrides
 * ─────────────────────────────────────────────────────────────────── */

bool CueArchive::hasFile(const Common::Path &path) const {
    Common::String name = path.baseName();
    return _files.contains(name) ||
           _files.contains(path.toString());
}

int CueArchive::listMembers(Common::ArchiveMemberList &list) const {
    int n = 0;
    for (auto it = _files.begin(); it != _files.end(); ++it) {
        list.push_back(getMember(Common::Path(it->_key)));
        ++n;
    }
    return n;
}

const Common::ArchiveMemberPtr CueArchive::getMember(const Common::Path &path) const {
    if (!hasFile(path)) return nullptr;
    return Common::ArchiveMemberPtr(new Common::GenericArchiveMember(path, *this));
}

Common::SeekableReadStream *
CueArchive::createReadStreamForMember(const Common::Path &path) const {
    Common::String name = path.baseName();
    auto it = _files.find(name);
    if (it == _files.end()) {
        it = _files.find(path.toString());
        if (it == _files.end()) return nullptr;
    }

    uint32 lba  = it->_value.lba;
    uint32 size = it->_value.size;
    uint32 byteStart = lba * kCookedSector;

    /* Multiple engine files may be open at once. SafeSeekableSubReadStream
     * re-seeks the shared cooked disc stream before each read, so interleaved
     * resource loads cannot corrupt each other's parent-stream position. */
    return new Common::SafeSeekableSubReadStream(_cooked, byteStart,
                                                 byteStart + size,
                                                 DisposeAfterUse::NO);
}

} // namespace OpenFPGA
