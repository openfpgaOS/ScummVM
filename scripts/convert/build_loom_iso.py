#!/usr/bin/env python3
"""build_loom_iso.py -- package Loom (VGA/CD) WITH music for the openfpgaOS port.

Loom VGA is SCUMM v4 "Loom CD": it has no MIDI -- its score is Red Book CD audio
that ScummVM reads from a single CDDA.SOU file (engines/scumm/cdda.cpp) and plays
through the mixer (the working PCM path, same as MI1 music). The stock data iso
has no CDDA.SOU, so music is silent.

This takes a raw full-disc Loom rip (data track + appended CD audio tracks) plus
the cooked data iso, transcodes the audio into CDDA.SOU at the exact origin the
engine expects, and rebuilds a cooked iso with the data files + CDDA.SOU.

ENGINE ORIGIN (critical): engines/scumm/script_v5.cpp:3567 maps a script CD
offset into CDDA.SOU as `offset*7.5 - 22500 - 2*75`, i.e. CDDA.SOU frame 0 is the
first audio sample AFTER the 22500-frame data track AND its 150-frame (2 s)
pregap leadin -- absolute frame 22650. So we start transcoding at frame 22650,
NOT at the end of the data track; including the pregap would shift every cue ~2 s
early and prepend 2 s of silence.

Pure-python (numpy for the audio): lib_iso9660 reads/writes the cooked isos,
lib_cdda does the CDDA.SOU transcode. No bsdtar/mkisofs/ffmpeg.

Usage:
  build_loom_iso.py --raw <loom.iso raw 2352> --data <loom_2048.iso cooked>
                    [--out DIR] [--keep]
"""
import sys
sys.dont_write_bytecode = True   # don't litter __pycache__ on the lib imports

import argparse
import os
import struct
import tempfile
import shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_cdda
import lib_iso9660

RAW = 2352            # raw MODE1/2352 sector
SYNC = 16             # sync+header before the 2048 cooked bytes
COOKED = 2048

# Engine-hardcoded CDDA.SOU origin (engines/scumm/script_v5.cpp:3567).
DATA_TRACK_FRAMES = 22500     # Loom CD data track (track 1) length, in frames
PREGAP_FRAMES = 150           # 2 s (2*75) audio-track leadin the engine subtracts
AUDIO_START_FRAME = DATA_TRACK_FRAMES + PREGAP_FRAMES   # 22650

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))   # scripts/convert -> repo root


def read_pvd_volume_blocks(raw_path):
    """Data-track length in 2048-byte logical blocks, from the PVD of a raw
    MODE1/2352 image (cooked PVD bytes start at 16*2352 + 16-byte sync)."""
    with open(raw_path, "rb") as f:
        f.seek(16 * RAW + SYNC)
        pvd = f.read(COOKED)
    if len(pvd) < 88 or pvd[0] != 1 or pvd[1:6] != b"CD001":
        sys.exit("ERROR: no ISO9660 PVD at LBA 16 -- is --raw a raw 2352 image?")
    return struct.unpack("<I", pvd[80:84])[0]


def main():
    ap = argparse.ArgumentParser(description="Package Loom CD with music (CDDA.SOU).")
    ap.add_argument("--raw", required=True, help="raw MODE1/2352 loom.iso (data + audio)")
    ap.add_argument("--data", required=True, help="cooked data iso (loom_2048.iso): LFL/LEC/EXE")
    ap.add_argument("--out", default=os.path.join(REPO_ROOT, "build", "loom"),
                    help="output dir for loom.iso (default: <repo>/build/loom)")
    ap.add_argument("--keep", action="store_true", help="keep the work dir")
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    size = os.path.getsize(args.raw)
    if size % RAW:
        sys.exit("ERROR: --raw size %d is not a multiple of %d (not a raw 2352 image)"
                 % (size, RAW))
    total_sectors = size // RAW

    data_blocks = read_pvd_volume_blocks(args.raw)
    if data_blocks != DATA_TRACK_FRAMES:
        print("WARNING: data track is %d frames but the Loom engine hardcodes %d "
              "(script_v5.cpp); music sync may be off for this nonstandard rip."
              % (data_blocks, DATA_TRACK_FRAMES), file=sys.stderr)

    n_audio = total_sectors - AUDIO_START_FRAME
    if n_audio <= 0:
        sys.exit("ERROR: no audio after frame %d (%d total sectors)"
                 % (AUDIO_START_FRAME, total_sectors))

    est_mb = (lib_cdda.HDR_SIZE + n_audio * lib_cdda.BLOCK_SIZE) // (1024 * 1024)
    print("[loom] data track %d frames | CDDA.SOU origin frame %d | %d audio blocks (~%d MB)"
          % (data_blocks, AUDIO_START_FRAME, n_audio, est_mb))

    work = tempfile.mkdtemp(prefix="loom-work-", dir=os.environ.get("SCRATCH") or None)
    try:
        # 1. Extract the data files (pure-python ISO read; fail fast before the
        #    expensive transcode if --data is wrong).
        files = lib_iso9660.extract_iso(args.data, os.path.join(work, "data"))
        print("[loom] data files: %s" % sorted(os.path.basename(f) for f in files))

        # 2. Transcode the audio tail -> CDDA.SOU at the engine's origin.
        sou = os.path.join(work, "CDDA.SOU")

        def progress(done, total):
            print("\r[loom] encoding CDDA.SOU... %d/%d blocks" % (done, total),
                  end="", flush=True)

        with open(args.raw, "rb") as src, open(sou, "wb") as out:
            lib_cdda.encode(src, out, AUDIO_START_FRAME * RAW, n_audio, progress=progress)
        print("\r[loom] CDDA.SOU: %d bytes%s" % (os.path.getsize(sou), " " * 24))

        # 3. Rebuild the cooked iso with data files + CDDA.SOU.
        files.append(sou)
        out_iso = os.path.join(out_dir, "loom.iso")
        lib_iso9660.build_iso(out_iso, [(os.path.basename(f), f) for f in files],
                              volid="LOOMCD")
        print("[loom] built %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))
        print("[loom] deploy: replace the Loom instance's slot-4 loom.iso with this "
              "file (keep loom_os.ini variant=VGA).")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
