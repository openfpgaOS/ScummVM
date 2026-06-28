#!/usr/bin/env python3
"""Decode MI1's Special-Edition music bank to port-playable CD-audio WAVs.

WHY
---
MI1 runs on the port as the SCUMM *SE* variant (GF_DOUBLEFINE_PAK), which routes
CD music through SoundSE -> audio/MusicOriginal.xwb.  That bank is mostly **xWMA**
(19 of 25 entries; the rest PCM), and the port has no working xWMA decoder
(audio/decoders/wmapro.cpp is an incomplete stub), so the SE path is silent.

The engine now falls back to the classic CD-audio manager when the SE stream is
null (engines/scumm/soundcd.cpp, NONSTANDARD_PORT), which plays `track<T>.wav`
files bundled in the ISO.  This script produces those WAVs by decoding the xWMA
*offline* with ffmpeg -- which, unlike the port, has a full xWMA/WMA decoder.

PIPELINE  (per engine CD track T the game requests)
---------------------------------------------------
  MusicOriginal.xwb  --lib_xwb.parse-->  wave entry "track{T+1}"   (the +1 is the
      Red Book vs engine-track offset; see lib_mi1_trackmap.py)
  entry  --lib_xwb.emit-->  intermediate  ( .xwma for WMA / .wav for PCM, both in
      a container stock ffmpeg reads)
  intermediate  --ffmpeg-->  track{T}.wav   (PCM s16le, 44100 Hz, stereo)

The output basenames (`trackT.wav`) are exactly what DefaultAudioCDManager /
the openfpga AudioCD manager open by name.

Usage:
  build_mi1_music.py <MusicOriginal.xwb> <outdir> [--rate HZ] [--mono]
Prints one produced path per line.  Exits non-zero if ffmpeg is missing or any
mapped track fails to decode (so the ISO build can fail loudly rather than ship
silent music).
"""
import sys, os, subprocess, tempfile, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_xwb
import lib_mi1_trackmap


def main():
    args = sys.argv[1:]
    rate = 44100
    channels = 2
    pcm = False
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--rate":
            rate = int(args[i + 1]); i += 2
        elif a == "--mono":
            channels = 1; i += 1
        elif a == "--pcm":
            pcm = True; i += 1
        else:
            pos.append(a); i += 1
    if len(pos) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    xwb, outdir = pos

    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ERROR: ffmpeg not found -- needed to decode MI1's xWMA music.\n"
              "  install hint: pacman -S ffmpeg", file=sys.stderr)
        sys.exit(3)

    os.makedirs(outdir, exist_ok=True)
    d, entries = lib_xwb.parse(xwb)
    by_name = {e["name"]: e for e in entries}

    # {trackT.wav: source_wave_name} for every engine-requested CD track.
    name_map = lib_mi1_trackmap.build_filename_map([e["name"] for e in entries])
    if not name_map:
        print("ERROR: no engine CD tracks mapped from %s (wrong bank?)" % xwb,
              file=sys.stderr)
        sys.exit(4)

    produced = []
    with tempfile.TemporaryDirectory() as tmp:
        for out_name in sorted(name_map, key=lambda s: int(s[5:-4])):
            wave_name = name_map[out_name]
            e = by_name.get(wave_name)
            if e is None:
                print("ERROR: mapped wave %r absent in bank" % wave_name,
                      file=sys.stderr)
                sys.exit(5)
            inter = lib_xwb.emit(d, e, tmp)          # .xwma or .wav
            out_path = os.path.join(outdir, out_name)
            # MS-ADPCM (~4:1) so the SD/bridge streams 4x fewer bytes during
            # play; the port decodes it via wave.cpp (kWaveFormatMSADPCM ->
            # makeADPCMStream).  Falls back to PCM if --pcm is given.
            codec = ["-c:a", "pcm_s16le"] if pcm else ["-c:a", "adpcm_ms"]
            cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                   "-i", inter, "-ar", str(rate), "-ac", str(channels)] \
                  + codec + [out_path]
            r = subprocess.run(cmd)
            if r.returncode != 0 or not os.path.exists(out_path):
                print("ERROR: ffmpeg failed for %s (%s)" % (out_name, wave_name),
                      file=sys.stderr)
                sys.exit(6)
            produced.append(out_path)
            print(out_path)

    print("# %d CD-audio WAVs written to %s (%d Hz, %s)" %
          (len(produced), outdir, rate, "mono" if channels == 1 else "stereo"),
          file=sys.stderr)


if __name__ == "__main__":
    main()
