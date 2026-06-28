#!/usr/bin/env python3
"""Assemble a single-file MODE1/2352 + CDDA .bin/.cue for the port (no deps).

Used for MI1 (gameid 'monkey', CD variant, GF_AUDIOTRACKS): the classic SCUMM
data goes on Track 01 as a MODE1/2352 data track wrapping an ISO9660 image,
and the decoded MusicOriginal music goes on Tracks 02..N as Red Book CDDA.

How the port reads this (verified against backend/openfpga_cue_archive.cpp and
backend/openfpga_audiocd.cpp):
  - The cue parser recognizes only FILE / TRACK <n> <TYPE> / INDEX <i> mm:ss:ff.
    PREGAP/POSTGAP are NOT parsed. A single FILE applies to all tracks.
  - Data track must be MODE1/2352 (or MODE1/RAW). dataStartByte = INDEX01_frame
    * 2352. Cooked bytes are read as _dataStart + sec*2352 + 16 + off, so only
    the 2048 user bytes per sector must be valid ISO data; the 16-byte sync+
    header and 288-byte EDC/ECC are never checked -> we zero-fill them.
  - Audio tracks are raw 2352-byte sectors of 44100 Hz / 16-bit LE / stereo PCM
    (Red Book). Each track's window is [byteOffset, nextTrackByteOffset) within
    the same FILE, so tracks must be contiguous and INDEX times are ABSOLUTE
    monotonically increasing disc positions.
  - play(track) -> physTrack = track + cd_track_offset -> matches TRACK <n>.
    With data=TRACK 01 and music=TRACK 02..N (original MI1 CD numbering, which
    is exactly what the SCUMM SOUN resource byte at +0x18 requests),
    cd_track_offset = 0.

API:
  wrap_mode1(iso_bytes) -> bytes (MODE1/2352)
  build_cuebin(out_bin, out_cue, iso_path, audio_tracks)
     audio_tracks: list of (track_number:int, pcm_path:str) where pcm is raw
     44100/16/stereo little-endian PCM (produced by ffmpeg). Must be sorted by
     track_number and contiguous; the data track is always TRACK 01.
"""
import struct, sys, os

RAW = 2352      # bytes per sector on disc (data and audio)
COOKED = 2048   # ISO9660 user bytes per MODE1 sector


def frames_to_msf(fr):
    return "%02d:%02d:%02d" % (fr // (75 * 60), (fr // 75) % 60, fr % 75)


def wrap_mode1(iso_bytes):
    """Cook 2048-byte ISO sectors into MODE1/2352 sectors.
    Zero the 16-byte sync+header and 288-byte EDC/ECC; the port ignores them."""
    out = bytearray()
    n = (len(iso_bytes) + COOKED - 1) // COOKED
    for i in range(n):
        chunk = iso_bytes[i * COOKED:(i + 1) * COOKED].ljust(COOKED, b'\x00')
        out += b'\x00' * 16 + chunk + b'\x00' * 288
    return bytes(out)


def build_cuebin(out_bin, out_cue, iso_path, audio_tracks, bin_basename=None):
    if bin_basename is None:
        bin_basename = os.path.basename(out_bin)
    data = wrap_mode1(open(iso_path, 'rb').read())
    bins = bytearray(data)
    frame = len(data) // RAW
    cue = ['FILE "%s" BINARY' % bin_basename,
           '  TRACK 01 MODE1/2352',
           '    INDEX 01 00:00:00']
    last = 1
    for num, pcm in sorted(audio_tracks, key=lambda t: t[0]):
        if num <= last:
            raise ValueError("audio track numbers must be increasing and >1: "
                             "got %d after %d" % (num, last))
        raw = open(pcm, 'rb').read()
        pad = (-len(raw)) % RAW
        if pad:
            raw = raw + b'\x00' * pad
        cue += ['  TRACK %02d AUDIO' % num,
                '    INDEX 01 %s' % frames_to_msf(frame)]
        bins += raw
        frame += len(raw) // RAW
        last = num
    with open(out_bin, 'wb') as o:
        o.write(bins)
    with open(out_cue, 'w') as o:
        o.write('\n'.join(cue) + '\n')
    return out_bin, out_cue


def main():
    # CLI: lib_cuebin.py <out.bin> <out.cue> <data.iso> [n=pcm ...]
    if len(sys.argv) < 4:
        print("usage: lib_cuebin.py <out.bin> <out.cue> <data.iso> "
              "[<tracknum>=<pcm> ...]", file=sys.stderr)
        sys.exit(2)
    out_bin, out_cue, iso = sys.argv[1], sys.argv[2], sys.argv[3]
    tracks = []
    for a in sys.argv[4:]:
        n, p = a.split('=', 1)
        tracks.append((int(n), p))
    build_cuebin(out_bin, out_cue, iso, tracks)
    print("wrote %s (%d bytes) and %s (%d audio tracks)" %
          (out_bin, os.path.getsize(out_bin), out_cue, len(tracks)))


if __name__ == '__main__':
    main()
