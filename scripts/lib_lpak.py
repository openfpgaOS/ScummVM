#!/usr/bin/env python3
"""Pure-Python extractor for LucasArts Special Edition 'KAPL'/LPAK .pak archives.

Used by build_monkey_iso.sh to pull the classic SCUMM v5 data out of the
Steam Special Edition .pak files (Monkey1.pak / monkey2.pak).

Layout (all little-endian, offsets relative to file start):
  Header (40 bytes):
    char[4] magic   = 'KAPL' (reads as "LPAK" reversed)
    float32 version = 1.0
    u32 startOfIndex
    u32 startOfFileEntries
    u32 startOfFileNames
    u32 startOfData
    u32 sizeOfIndex
    u32 sizeOfFileEntries   (== numEntries * 20)
    u32 sizeOfFileNames
    u32 sizeOfData
  Index (at startOfIndex, sizeOfIndex == numEntries*4):
    u32[numEntries] sorted hash table for fast name lookup. NOT needed for
    extraction (values are ~32-bit hashes, not offsets). Ignored here.
  File entry (20 bytes each, at startOfFileEntries):
    u32 fileDataPos   (add startOfData for absolute file offset)
    u32 fileNamePos   (NOT a usable byte offset in MI2; see note below)
    u32 dataSize
    u32 dataSize2     (== dataSize; uncompressed size)
    u32 compressed    (always 0 in MISE/MI2SE -> stored uncompressed)
  Names: NUL-terminated strings packed at startOfFileNames, one per entry.

  IMPORTANT: the i-th NUL-terminated name in the name table belongs to the
  i-th file entry (sequential pairing, exactly as ScummVM's own loader does in
  engines/scumm/file_engine.cpp). The fileNamePos field is a raw byte offset
  only in Monkey1.pak (entries happen to be name-sorted there); in
  monkey2.pak fileNamePos does NOT land on a name boundary, so we MUST pair
  sequentially and ignore fileNamePos.

  Validated: the paired classic/en/monkey1.001 (and monkey2.001) data XOR 0x69
  -> 'LECF...LOFF' and the .000 -> 'RNAM...' (correct SCUMM v5 blocks). The
  classic resources are left XOR-0x69 encrypted exactly as the floppy/CD
  originals; ScummVM applies the 0x69 key itself, so do NOT decrypt here.

CLI:
  lib_lpak.py list <pak>
  lib_lpak.py extract <pak> <dest> [name-substr ...]
"""
import struct, sys, os

class Entry:
    __slots__ = ("name", "offset", "size", "size2", "compressed")

class LPak:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(40)
        if hdr[:4] != b"KAPL":
            raise ValueError("not a KAPL/LPAK file: %r" % hdr[:4])
        (self.version,) = struct.unpack("<f", hdr[4:8])
        (self.soi, self.sfe, self.sfn, self.sod,
         self.szi, self.szfe, self.szfn, self.szd) = struct.unpack("<8I", hdr[8:40])
        self.num = self.szfe // 20
        # read name table once; names are NUL-terminated, one per entry,
        # paired by sequential order with file entries.
        self.f.seek(self.sfn)
        self.names_blob = self.f.read(self.szfn)
        names = self.names_blob.split(b"\x00")
        if names and names[-1] == b"":
            names.pop()
        if len(names) != self.num:
            raise ValueError("name count %d != entry count %d" % (len(names), self.num))
        # read entries
        self.f.seek(self.sfe)
        ent_blob = self.f.read(self.szfe)
        self.entries = []
        for i in range(self.num):
            dpos, npos, sz, sz2, comp = struct.unpack_from("<5I", ent_blob, i * 20)
            e = Entry()
            e.name = names[i].decode("latin-1")
            e.offset = self.sod + dpos
            e.size = sz
            e.size2 = sz2
            e.compressed = comp
            self.entries.append(e)

    def read(self, e):
        self.f.seek(e.offset)
        return self.f.read(e.size)

    def find(self, substr):
        """Return entries whose name contains substr (case-insensitive)."""
        s = substr.lower()
        return [e for e in self.entries if s in e.name.lower()]

    def extract(self, e, dest_root, flatten=False):
        name = os.path.basename(e.name) if flatten else e.name.replace("\\", "/")
        out = os.path.join(dest_root, name)
        d = os.path.dirname(out)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(out, "wb") as o:
            o.write(self.read(e))
        return out


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        print("usage: lib_lpak.py list <pak>\n"
              "       lib_lpak.py extract <pak> <dest> [name-substr ...]",
              file=sys.stderr)
        sys.exit(2)
    cmd, pak = sys.argv[1], sys.argv[2]
    p = LPak(pak)
    if cmd == "list":
        print("# %s  version=%.1f  entries=%d" % (pak, p.version, p.num))
        print("# any-compressed=%s" % any(e.compressed for e in p.entries))
        for e in p.entries:
            flag = "C" if e.compressed else " "
            print("%s %10d  %s" % (flag, e.size, e.name))
    else:
        dest = sys.argv[3]
        subs = sys.argv[4:]
        n = 0
        for e in p.entries:
            if subs and not any(s.lower() in e.name.lower() for s in subs):
                continue
            p.extract(e, dest)
            n += 1
        print("extracted %d files to %s" % (n, dest))

if __name__ == "__main__":
    main()
