#!/usr/bin/env python3
"""Minimal pure-Python ISO9660 *level 1* image builder (no external deps).

Why hand-rolled: the build host has no mkisofs/genisoimage/xorriso and no
pycdlib, and the port's app-side parser (backend/openfpga_cue_archive.cpp,
parseISO9660) reads ONLY the Primary Volume Descriptor at LBA 16 and the
standard 8.3 directory records -- it ignores Joliet / Rock Ridge / SVD / El
Torito entirely. The kernel of_iso_mount() likewise expects plain ISO9660.
So we emit ISO9660 level 1 only: 8.3 uppercase names, ';1' version, files at
root, 2048-byte logical sectors. That is exactly what MI1/MI2 classic data
plus extracted CD-audio tracks need (MONKEY1.000/.001, TRACK2.WAV..TRACK25.WAV).

This builder supports a *multi-sector root directory* and *large files*
(32-bit extent LBA + 32-bit size, both-endian) so the MI1 disc -- ~26 files,
several 10-20 MB WAVs, ~500 MB total -- builds correctly. Directory records
never cross a 2048-byte boundary; the root extent is padded to whole sectors;
file extents are sector-aligned. Files are streamed in 1 MiB chunks so peak
RAM stays small regardless of total size.

Layout produced:
  LBA  0..15 : 16 system-area sectors (zero)
  LBA 16     : Primary Volume Descriptor
  LBA 17     : Volume Descriptor Set Terminator
  LBA 18     : Little-endian path table
  LBA 19     : Big-endian path table
  LBA 20..   : Root directory extent (>=1 sector)
  LBA ..     : file extents (each file sector-aligned)

Neither consumer (port parseISO9660, kernel lib9660) reads the L/M path
tables; they are emitted (root-only) purely for standards compliance.

Validated shape against the engine's parseISO9660 expectations: PVD at sector
16, root dir record inside the PVD at offset 156 (34 bytes), extent LBA as
LE32 @ rec+2, size as LE32 @ rec+10, flags @ rec+25, nameLen @ rec+32, 8.3
uppercase names with a ';1' version suffix.

API:
  build_iso(out_path, [(arcname, src_path), ...], volid='SCUMMVM')
CLI:
  lib_iso9660.py <out.iso> <volid> <file1> [file2 ...]
"""
import struct, sys, os, datetime

SECTOR = 2048


def _both_u16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def _both_u32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def _dec_datetime(dt=None):
    # 17-byte dec-datetime for the PVD.
    if dt is None:
        dt = datetime.datetime(2000, 1, 1, 0, 0, 0)
    s = "%04d%02d%02d%02d%02d%02d00" % (dt.year, dt.month, dt.day,
                                        dt.hour, dt.minute, dt.second)
    return s.encode('ascii') + bytes([0])  # GMT offset 0


def _dir_datetime(dt=None):
    # 7-byte dir-record datetime.
    if dt is None:
        dt = datetime.datetime(2000, 1, 1, 0, 0, 0)
    return bytes([dt.year - 1900, dt.month, dt.day, dt.hour,
                  dt.minute, dt.second, 0])


def _iso_name(arcname):
    """Normalize to an uppercased ISO9660 identifier with a ';1' version.

    NOTE: we intentionally do NOT enforce strict level-1 8.3 length limits.
    The two consumers of these images -- the port's parseISO9660
    (backend/openfpga_cue_archive.cpp) and the kernel of_iso_mount -- read
    nameLen + the raw identifier bytes and only strip the ';1' version
    suffix; neither truncates to 8.3.  Enforcing 8.3 here would corrupt the
    SE speech-bank filenames the SCUMM engine opens by exact basename
    (e.g. `speech.info` -> `SPEECH.INF`, `SpeechCues.xsb` -> `SPEECHCU.XSB`),
    which would silently disable SE voices.  We keep uppercasing, the
    A-Z/0-9/_/. charset map, and the ';1' suffix; we do not shorten.

    The directory-record writer already pads any record to even length and
    keeps records from crossing a 2048-byte sector boundary, so long names
    are safe.  Identifier length is a single byte, so names must stay
    < ~220 chars (all SE banks are far shorter)."""
    base = os.path.basename(arcname).upper()
    if '.' in base:
        stem, ext = base.rsplit('.', 1)
    else:
        stem, ext = base, ''
    # charset A-Z 0-9 _ . ; map anything else to _  (no length truncation)
    def clean(s):
        return ''.join(c if (c.isalnum() or c == '_') else '_' for c in s)
    stem = clean(stem)
    ext = clean(ext)
    ident = stem + ('.' + ext if ext else '.')  # keep a trailing dot like L1
    return (ident + ';1').encode('ascii')


def _dir_record(name_bytes, lba, length, flags, dt=None):
    # length-of-identifier and padding
    nlen = len(name_bytes)
    rec_len = 33 + nlen
    if rec_len % 2:
        rec_len += 1  # pad to even
    r = bytearray()
    r.append(rec_len)            # length of directory record
    r.append(0)                  # extended attr record length
    r += _both_u32(lba)          # extent location
    r += _both_u32(length)       # data length
    r += _dir_datetime(dt)       # recording date/time (7)
    r.append(flags)              # file flags (0x02 = directory)
    r.append(0)                  # file unit size
    r.append(0)                  # interleave gap size
    r += _both_u16(1)            # volume sequence number
    r.append(nlen)               # length of file identifier
    r += name_bytes
    if len(r) < rec_len:
        r += b'\x00' * (rec_len - len(r))
    return bytes(r)


def _build_root_records(items, root_lba, dt):
    """Build the root directory extent payload (multi-sector aware).

    Returns (root_bytes_padded_to_whole_sectors, root_extent_len).

    The '.' and '..' records reference the root and must report the WHOLE
    padded extent length as their size. The record byte-lengths of '.'/'..'
    are fixed (single-byte name), so the overall layout does not depend on
    the size value -- we discover the extent length in pass 1 (placeholder
    size) and re-emit it in pass 2 with the correct '.'/'..' size; both
    passes are identical in length (asserted)."""
    def emit(size_for_dotdirs):
        buf = bytearray()
        buf += _dir_record(b'\x00', root_lba, size_for_dotdirs, 0x02, dt)  # '.'
        buf += _dir_record(b'\x01', root_lba, size_for_dotdirs, 0x02, dt)  # '..'
        for it in items:
            rec = _dir_record(it['iso'], it['lba'], it['size'], 0x00, dt)
            # directory records must not cross a 2048 boundary -> pad sector
            if (len(buf) % SECTOR) + len(rec) > SECTOR:
                buf += b'\x00' * (SECTOR - (len(buf) % SECTOR))
            buf += rec
        # pad the whole extent to a sector multiple
        if len(buf) % SECTOR:
            buf += b'\x00' * (SECTOR - len(buf) % SECTOR)
        return buf
    tmp = emit(SECTOR)               # pass 1: discover extent length
    extent_len = len(tmp)
    final = emit(extent_len)         # pass 2: correct '.'/'..' size
    assert len(final) == extent_len, (len(final), extent_len)
    return final, extent_len


def build_iso(out_path, files, volid='SCUMMVM'):
    """files: list of (arcname, src_path). Single flat root directory."""
    # Resolve file sizes.
    items = []
    for arcname, src in files:
        sz = os.path.getsize(src)
        items.append(dict(iso=_iso_name(arcname), src=src, size=sz))
    # ISO L1 nominally sorts directory entries by identifier; neither consumer
    # requires it, but sort for spec-correctness and deterministic output.
    items.sort(key=lambda it: it['iso'])

    # Fixed-position sectors.
    PVD_LBA = 16
    TERM_LBA = 17
    PT_LE_LBA = 18
    PT_BE_LBA = 19
    ROOT_LBA = 20

    dt = datetime.datetime(2000, 1, 1)

    # Determine how many sectors the root extent needs. The root extent byte
    # length depends only on the file COUNT/names (record sizes are fixed-
    # width), NOT on the file LBAs -- so compute it once with placeholder
    # LBAs, derive root_sectors, then assign real file LBAs after the root,
    # then re-emit the root with the real LBAs (same length, asserted).
    for it in items:
        it['lba'] = 0  # placeholder
    _root_tmp, root_extent_len = _build_root_records(items, ROOT_LBA, dt)
    root_sectors = root_extent_len // SECTOR

    first_file_lba = ROOT_LBA + root_sectors
    lba = first_file_lba
    for it in items:
        it['lba'] = lba
        nsec = (it['size'] + SECTOR - 1) // SECTOR
        it['nsec'] = nsec
        lba += nsec
    total_sectors = lba if items else first_file_lba

    root_data, root_extent_len2 = _build_root_records(items, ROOT_LBA, dt)
    assert root_extent_len2 == root_extent_len, (root_extent_len2, root_extent_len)

    # --- Path tables (just the root) ---
    # path table record: dir-id len(1), ext-attr-len(1), extent(4), parent(2), id
    def path_table(big):
        ident = b'\x00'  # root id is a single 0 byte
        nlen = 1
        if big:
            ext = struct.pack('>I', ROOT_LBA)
            par = struct.pack('>H', 1)
        else:
            ext = struct.pack('<I', ROOT_LBA)
            par = struct.pack('<H', 1)
        rec = bytes([nlen, 0]) + ext + par + ident
        if len(rec) % 2:
            rec += b'\x00'
        return rec
    pt_le = path_table(False)
    pt_be = path_table(True)
    pt_size = len(pt_le)  # same as be

    # --- Primary Volume Descriptor (sector 16) ---
    pvd = bytearray(b'\x00' * SECTOR)
    pvd[0] = 1                                   # type = primary
    pvd[1:6] = b'CD001'                          # standard identifier
    pvd[6] = 1                                   # version
    # system id (32, A-chars), volume id (32, D-chars)
    pvd[8:8 + 32] = b' ' * 32
    vid = (volid.upper().encode('ascii')[:32]).ljust(32, b' ')
    pvd[40:40 + 32] = vid
    # volume space size (both-endian u32) at 80
    pvd[80:88] = _both_u32(total_sectors)
    # volume set size (both u16) at 120, sequence number at 124
    pvd[120:124] = _both_u16(1)
    pvd[124:128] = _both_u16(1)
    # logical block size (both u16) at 128
    pvd[128:132] = _both_u16(SECTOR)
    # path table size (both u32) at 132
    pvd[132:140] = _both_u32(pt_size)
    # type-L path table LBA (LE u32) at 140; optional at 144 (0)
    pvd[140:144] = struct.pack('<I', PT_LE_LBA)
    pvd[144:148] = struct.pack('<I', 0)
    # type-M path table LBA (BE u32) at 148; optional at 152 (0)
    pvd[148:152] = struct.pack('>I', PT_BE_LBA)
    pvd[152:156] = struct.pack('>I', 0)
    # root directory record (34 bytes) at 156
    root_rec = _dir_record(b'\x00', ROOT_LBA, root_extent_len, 0x02, dt)
    assert len(root_rec) == 34, len(root_rec)
    pvd[156:156 + 34] = root_rec
    # volume set / publisher / preparer / application ids (128 each) -> spaces
    for off in (190, 318, 446, 574):
        pvd[off:off + 128] = b' ' * 128
    # file identifiers (37 each) at 702,739,776 -> spaces
    for off in (702, 739, 776):
        pvd[off:off + 37] = b' ' * 37
    # volume creation/modification/expiration/effective dates (17 each)
    cdt = _dec_datetime(dt)
    pvd[813:813 + 17] = cdt
    pvd[830:830 + 17] = cdt
    pvd[847:847 + 17] = b'0' * 16 + bytes([0])   # expiration: none
    pvd[864:864 + 17] = cdt
    pvd[881] = 1                                  # file structure version

    # --- Volume Descriptor Set Terminator (sector 17) ---
    term = bytearray(b'\x00' * SECTOR)
    term[0] = 0xFF
    term[1:6] = b'CD001'
    term[6] = 1

    # --- Path table sectors ---
    pt_le_sec = pt_le.ljust(SECTOR, b'\x00')
    pt_be_sec = pt_be.ljust(SECTOR, b'\x00')

    # --- Assemble ---
    with open(out_path, 'wb') as o:
        o.write(b'\x00' * (SECTOR * 16))         # system area (LBA 0..15)
        o.write(pvd)                              # 16
        o.write(term)                             # 17
        o.write(pt_le_sec)                        # 18
        o.write(pt_be_sec)                        # 19
        o.write(bytes(root_data))                 # 20.. (padded to whole sectors)
        assert len(root_data) == root_sectors * SECTOR, \
            (len(root_data), root_sectors * SECTOR)
        # files (streamed in chunks; sector-aligned by trailing pad)
        for it in items:
            with open(it['src'], 'rb') as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    o.write(chunk)
            pad = (SECTOR - it['size'] % SECTOR) % SECTOR
            if pad:
                o.write(b'\x00' * pad)
    return out_path


def main():
    if len(sys.argv) < 4:
        print("usage: lib_iso9660.py <out.iso> <volid> <file1> [file2 ...]",
              file=sys.stderr)
        sys.exit(2)
    out = sys.argv[1]; volid = sys.argv[2]; srcs = sys.argv[3:]
    build_iso(out, [(s, s) for s in srcs], volid=volid)
    print("wrote %s (%d bytes) volid=%s files=%s" %
          (out, os.path.getsize(out), volid, [os.path.basename(s) for s in srcs]))


if __name__ == '__main__':
    main()
