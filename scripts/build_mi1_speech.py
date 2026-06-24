#!/usr/bin/env python3
"""Repack MI1's SE Speech.xwb to shrink it -- MS-ADPCM (default) or downsampled PCM.

MI1 ships Speech.xwb as ~838 MB of 44.1 kHz 16-bit PCM (4551 clips), played
verbatim through SoundSE.  On the handheld that's wasted SD/bridge bandwidth.
SoundSE reads each clip's codec/rate/blockAlign straight from the wave-bank
metadata, so we can re-encode the clips and just rewrite the metadata -- no
engine change.

  default (MS-ADPCM): ~3.7:1.  Encoded with ffmpeg to the SAME block format MI2's
      Speech.xwb already uses and the port already decodes: mono, 128 samples /
      70-byte block (XWB alignField 48, codec 2).  Add --rate to also downsample
      (e.g. 22050 -> ~7:1).
  --pcm: keep PCM, just downsample/mono (numpy).  ~2:1 at 22050, ~4:1 at 11025.

Only the entry metadata + wave-data segment are rewritten; the WBND header,
BankData and the entry-NAMES segment are copied byte-for-byte, so the bank stays
a valid WBND the engine indexes identically (same count+order -> same
speech.info match).  Non-PCM source entries are copied untouched.

Usage:
  build_mi1_speech.py <in Speech.xwb> <out Speech.xwb> [--rate HZ] [--mono] [--pcm]
"""
import sys, os, struct, subprocess, shutil, tempfile
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_xwb

PCM_ALIGN = 2048           # XWB wave-entry alignment for the --pcm path
# ffmpeg requires adpcm_ms -block_size to be a power of two, so we can't match
# MI2's 70-byte block exactly; 128 is the next valid size and the port's generic
# kADPCMMS decoder handles any blockAlign.  Mono 128-byte block = 7-byte header +
# 121 data bytes = 244 samples (nBlockAlign 7 + (spb-2)/2).
ADPCM_NBLOCKALIGN = 128
ADPCM_SAMPLES_PER_BLOCK = 2 + (ADPCM_NBLOCKALIGN - 7) * 2   # = 244
ADPCM_ALIGN_FIELD = ADPCM_NBLOCKALIGN - 22                  # = 106 (engine: (align+22)*ch)


def make_format(codec, ch, rate, block_align_field, bits16):
    return (codec & 3) | ((ch & 7) << 2) | ((rate & 0x3FFFF) << 5) \
        | ((block_align_field & 0xFF) << 23) | ((bits16 & 1) << 31)


def to_mono(pcm_i16, ch):
    a = pcm_i16.astype(np.float32)
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1)
    return a


def resample(col, src_rate, dst_rate):
    if dst_rate == src_rate or col.shape[0] == 0:
        return col
    win = max(1, int(round(src_rate / float(dst_rate))))
    if win > 1:
        k = np.ones(win, dtype=np.float32) / win
        col = np.convolve(col, k, mode="same")
    new_n = max(1, int(round(col.shape[0] * dst_rate / float(src_rate))))
    xp = np.arange(col.shape[0], dtype=np.float32)
    x = np.linspace(0, col.shape[0] - 1, new_n, dtype=np.float32)
    return np.interp(x, xp, col).astype(np.float32)


def f32_to_i16(a):
    np.clip(a, -32768, 32767, out=a)
    return a.astype("<i2")


def rewrite(raw, meta_off, metaSize, idx, fad_samples, fmt, po, length):
    mo = meta_off + idx * metaSize
    orig_fad = struct.unpack_from("<I", raw, mo)[0]
    new_fad = ((fad_samples << 4) & 0xFFFFFFF0) | (orig_fad & 0xF)
    struct.pack_into("<IIII", raw, mo, new_fad, fmt, po, length)


def build_pcm(raw, d, entries, meta_off, metaSize, rate, mono):
    """numpy PCM downsample path (codec 0)."""
    new_wave = bytearray()
    for e in entries:
        body = bytes(raw[e["po"]:e["po"] + e["pl"]])
        if e["codec"] != 0:
            data = body
            fmt = struct.unpack_from("<I", raw, meta_off + e["idx"] * metaSize + 4)[0]
            nsamp = e["pl"]
        else:
            a = to_mono(np.frombuffer(body, dtype="<i2"), e["ch"]) if (mono or e["ch"] == 1) \
                else np.frombuffer(body, dtype="<i2").astype(np.float32)
            ch_out = 1 if (mono or e["ch"] == 1) else e["ch"]
            if ch_out == 1:
                a = resample(a, e["rate"], rate)
                out = f32_to_i16(a)
            else:
                cols = [resample(a.reshape(-1, ch_out)[:, c], e["rate"], rate) for c in range(ch_out)]
                out = f32_to_i16(np.stack(cols, axis=1).reshape(-1))
            data = out.tobytes()
            fmt = make_format(0, ch_out, rate, ch_out * 2, 1)
            nsamp = len(data) // 2
        if len(new_wave) % PCM_ALIGN:
            new_wave += b"\0" * (PCM_ALIGN - (len(new_wave) % PCM_ALIGN))
        po = len(new_wave)
        new_wave += data
        rewrite(raw, meta_off, metaSize, e["idx"], nsamp, fmt, po, len(data))
    return new_wave


def build_adpcm(raw, d, entries, meta_off, metaSize, rate):
    """MS-ADPCM path (codec 2): batch-encode block-aligned mono PCM with ffmpeg."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("ERROR: ffmpeg not found (needed for MS-ADPCM encode)")
    dst_rate = rate or entries[0]["rate"]
    SPB = ADPCM_SAMPLES_PER_BLOCK

    tmp = tempfile.mkdtemp(prefix="mi1adpcm_")
    pcm_path = os.path.join(tmp, "all.s16")
    plan = []            # per PCM entry: (idx, block_count)
    try:
        with open(pcm_path, "wb") as pf:
            for e in entries:
                if e["codec"] != 0:
                    plan.append((e["idx"], None))      # non-PCM: copy later
                    continue
                a = to_mono(np.frombuffer(bytes(raw[e["po"]:e["po"] + e["pl"]]), dtype="<i2"), e["ch"])
                a = resample(a, e["rate"], dst_rate)
                out = f32_to_i16(a)
                nblk = (len(out) + SPB - 1) // SPB
                pad = nblk * SPB - len(out)
                if pad:
                    out = np.concatenate([out, np.zeros(pad, dtype="<i2")])
                pf.write(out.tobytes())
                plan.append((e["idx"], nblk))

        adpcm_path = os.path.join(tmp, "all.wav")
        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
               "-f", "s16le", "-ar", str(dst_rate), "-ac", "1", "-i", pcm_path,
               "-c:a", "adpcm_ms", "-block_size", str(ADPCM_NBLOCKALIGN), adpcm_path]
        if subprocess.run(cmd).returncode != 0:
            sys.exit("ERROR: ffmpeg adpcm_ms encode failed")

        wav = open(adpcm_path, "rb").read()
        # pull nBlockAlign + the data chunk
        fi = wav.find(b"fmt ")
        nba = struct.unpack_from("<H", wav, fi + 8 + 12)[0]
        if nba != ADPCM_NBLOCKALIGN:
            sys.exit("ERROR: ffmpeg produced nBlockAlign=%d, expected %d" % (nba, ADPCM_NBLOCKALIGN))
        di = wav.find(b"data", fi)
        dlen = struct.unpack_from("<I", wav, di + 4)[0]
        adpcm = wav[di + 8: di + 8 + dlen]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    fmt_adpcm = make_format(2, 1, dst_rate, ADPCM_ALIGN_FIELD, 0)
    new_wave = bytearray()
    blk_cursor = 0
    for idx, nblk in plan:
        if nblk is None:                                   # non-PCM: copy verbatim
            e = entries[idx]
            data = bytes(raw[e["po"]:e["po"] + e["pl"]])
            fmt = struct.unpack_from("<I", raw, meta_off + idx * metaSize + 4)[0]
            po = len(new_wave); new_wave += data
            rewrite(raw, meta_off, metaSize, idx, e["pl"], fmt, po, len(data))
            continue
        length = nblk * ADPCM_NBLOCKALIGN
        chunk = adpcm[blk_cursor * ADPCM_NBLOCKALIGN: blk_cursor * ADPCM_NBLOCKALIGN + length]
        blk_cursor += nblk
        po = len(new_wave); new_wave += chunk
        rewrite(raw, meta_off, metaSize, idx, nblk * SPB, fmt_adpcm, po, length)
    if blk_cursor * ADPCM_NBLOCKALIGN != len(adpcm):
        sys.exit("ERROR: block accounting mismatch (%d vs %d)"
                 % (blk_cursor * ADPCM_NBLOCKALIGN, len(adpcm)))
    return new_wave


def main():
    args = sys.argv[1:]
    rate = None
    mono = False
    pcm = False
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--rate":
            rate = int(args[i + 1]); i += 2
        elif args[i] == "--mono":
            mono = True; i += 1
        elif args[i] == "--pcm":
            pcm = True; i += 1
        else:
            pos.append(args[i]); i += 1
    if len(pos) != 2:
        print(__doc__, file=sys.stderr); sys.exit(2)
    src, dst = pos

    raw = bytearray(open(src, "rb").read())
    d, entries = lib_xwb.parse(src)
    segs = [struct.unpack_from("<II", raw, 12 + 8 * k) for k in range(5)]
    meta_off = segs[1][0]
    wave_off = segs[4][0]
    metaSize = struct.unpack_from("<I", raw, segs[0][0] + 8 + 64)[0]

    if pcm:
        new_wave = build_pcm(raw, d, entries, meta_off, metaSize, rate or 22050, mono)
        mode = "PCM %dHz%s" % (rate or 22050, " mono" if mono else "")
    else:
        new_wave = build_adpcm(raw, d, entries, meta_off, metaSize, rate)
        mode = "MS-ADPCM%s mono" % ((" %dHz" % rate) if rate else "")

    head = raw[:wave_off]
    struct.pack_into("<I", head, 12 + 4 * 8 + 4, len(new_wave))   # WaveData segment length
    with open(dst, "wb") as f:
        f.write(head)
        f.write(new_wave)

    print("%s: %d entries  %d -> %d bytes (%.0f%%)  %s" % (
        os.path.basename(dst), len(entries), os.path.getsize(src),
        os.path.getsize(dst), 100.0 * os.path.getsize(dst) / os.path.getsize(src), mode),
        file=sys.stderr)


if __name__ == "__main__":
    main()
