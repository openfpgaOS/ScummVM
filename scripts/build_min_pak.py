#!/usr/bin/env python3
"""Build a minimal KAPL/LPAK .pak containing only selected files.

The LucasArts SE/remaster container (Monkey1.pak = 1.24 GB, 2599 files) bundles
remastered art/audio/etc., but in classic mode the engine reads only a couple of
files from it (e.g. `classic/en/monkey1.000` + `.001`, ~4.8 MB) via ScummPAKFile.
Shipping the whole 1.24 GB just bloats the ISO.  This repacks a new KAPL with
ONLY the matched entries, byte-for-byte (data stays XOR-encrypted exactly as the
engine expects), so the ISO drops from gigabytes to a few MB.

It stays a valid KAPL that ScummPAKFile::readIndex parses identically (same entry
records, full names preserved so the engine's "classic/"-prefix filter still
matches).  Compressed source entries are rejected (the engine doesn't inflate).

Usage:
  build_min_pak.py <src.pak> <out.pak> <name-substr> [more-substr ...]
Selects entries whose name contains ANY substring (case-insensitive).
"""
import sys, os, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_lpak


def main():
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr); sys.exit(2)
    src, out = sys.argv[1], sys.argv[2]
    subs = [s.lower() for s in sys.argv[3:]]

    p = lib_lpak.LPak(src)
    sel = [e for e in p.entries if any(s in e.name.lower() for s in subs)]
    if not sel:
        print("ERROR: no entries match %s in %s" % (subs, src), file=sys.stderr)
        sys.exit(4)
    bad = [e.name for e in sel if e.compressed]
    if bad:
        print("ERROR: selected entries are compressed (engine can't inflate): %s"
              % bad, file=sys.stderr)
        sys.exit(5)

    # Layout: header(40) | entry table | name table | data
    n = len(sel)
    sfe = 40
    szfe = n * 20
    sfn = sfe + szfe
    names_blob = b"".join(e.name.encode("latin-1") + b"\0" for e in sel)
    szfn = len(names_blob)
    sod = (sfn + szfn + 15) & ~15           # 16-align the data region
    pad_names = sod - (sfn + szfn)

    # Concatenate file data; record each entry's offset (relative to sod).
    datas = [p.read(e) for e in sel]
    entry_recs = bytearray()
    name_off = 0
    data_off = 0
    for e, d in zip(sel, datas):
        # dpos(rel sod), npos(rel sfn), size, size2, compressed
        entry_recs += struct.pack("<5I", data_off, name_off, e.size, e.size, 0)
        name_off += len(e.name) + 1
        data_off += len(d)
    szd = data_off

    hdr = b"KAPL"
    hdr += struct.pack("<f", p.version)
    # soi, sfe, sfn, sod, szi, szfe, szfn, szd
    hdr += struct.pack("<8I", sfe, sfe, sfn, sod, szfe + szfn, szfe, szfn, szd)

    with open(out, "wb") as f:
        f.write(hdr)
        f.write(entry_recs)
        f.write(names_blob)
        f.write(b"\0" * pad_names)
        for d in datas:
            f.write(d)

    print("%s: %d files, %d bytes (from %d-file %.0fMB source)" % (
        os.path.basename(out), n, os.path.getsize(out), p.num,
        os.path.getsize(src) / 1048576), file=sys.stderr)
    for e in sel:
        print("  %-28s %d" % (e.name, e.size), file=sys.stderr)


if __name__ == "__main__":
    main()
