#!/usr/bin/env python3
"""Pure-python XACT3 WBND (.xwb) wave-bank extractor for the Monkey Island SE
faithful re-recorded music (audio/MusicOriginal.xwb).

It parses the WBND segments and writes each entry out in a form stock ffmpeg
can decode:
  - PCM entries          -> plain RIFF/WAVE
  - WMA (xWMA) entries    -> RIFF 'XWMA' (fmt WAVEFORMATEX tag 0x0161 + dpds +
                             data); ffmpeg's xwma demuxer reads this directly.
  - MS-ADPCM entries      -> RIFF/WAVE with MS-ADPCM fmt (tag 0x0002 + coef
                             block); ffmpeg decodes natively.

No vgmstream required.

WBND format (little-endian), verified against the real files:
  0x00 'WBND'  0x04 dwVersion  0x08 dwHeaderVersion
  0x0C 5x WAVEBANKREGION (u32 offset, u32 length):
        [0]BANKDATA [1]ENTRYMETADATA [2]SEEKTABLES [3]ENTRYNAMES [4]ENTRYWAVEDATA
  BANKDATA: dwFlags, dwEntryCount, szBankName[64],
            dwEntryMetaDataElementSize, dwEntryNameElementSize, dwAlignment, ...
  ENTRYMETADATA: dwEntryCount x (metaSize-byte) WAVEBANKENTRY:
        u32 dwFlagsAndDuration, u32 Format, PlayRegion{u32 off,u32 len},
        LoopRegion{u32 off,u32 len}.  PlayRegion.off is relative to the
        ENTRYWAVEDATA segment start.
  Format dword bit-packing (v2+):
        codec=bits0-1, channels=bits2-4, samplesPerSec=bits5-22 (18b),
        blockAlign=bits23-30 (8b), bitsPerSample=bit31.
        codec: 0=PCM 1=XMA 2=ADPCM(MS) 3=WMA(xWMA).
        For WMA, blockAlign is an INDEX:
          nBlockAlign      = aWMABlockAlign[blk & 0x1F]
          nAvgBytesPerSec  = aWMAAvgBytesPerSec[(blk>>5)&7]
  SEEKTABLES (present for WMA): count u32 offsets (0xFFFFFFFF=none) then
        per-entry [u32 packetCount][packetCount x u32] = the dpds table
        (cumulative decoded bytes per xWMA packet).
  ENTRYNAMES: count x dwEntryNameElementSize NUL-padded names (may be absent;
        MI2's bank has no names -> we synthesize "entryNN").

MI1 audio/MusicOriginal.xwb: WBND v45, 25 entries, names track2..track25 +
  'silence'. Entries 0-18 = xWMA 44100/stereo; 19-24 = PCM 16-bit;
  entry 'silence' = PCM mono filler (SE-only, no original CD track).
MI2 audio/MusicOriginal.xwb: WBND v46, 267 entries, all MS-ADPCM, no names,
  no seektables. (MI2's intended music route is iMUSE MIDI; this bank is only
  a fallback.)

CLI:
  lib_xwb.py list <xwb>
  lib_xwb.py extract <xwb> <outdir> [name]   # name optional: single entry
"""
import sys, struct, os

# XACT WMA lookup tables (from DirectXTK / xact3wb.h)
aWMABlockAlign = [929, 1487, 1280, 2230, 8917, 8192, 4459, 5945, 2304, 1536,
                  1485, 1008, 2731, 4096, 6827, 5462, 1280]
aWMAAvgBytesPerSec = [12000, 24000, 4000, 6000, 8000, 20000, 2500]

CODEC = ['PCM', 'XMA', 'ADPCM', 'WMA']

# Standard MS-ADPCM 7-coefficient table (wFormatTag 0x0002).
MSADPCM_COEFS = [(256, 0), (512, -256), (0, 0), (192, 64),
                 (240, 0), (460, -208), (392, -232)]


def parse(path):
    d = open(path, 'rb').read()
    if d[0:4] != b'WBND':
        raise ValueError("not WBND: %r" % d[0:4])
    ver, hver = struct.unpack('<II', d[4:12])
    segs = [struct.unpack('<II', d[0x0c + i * 8:0x0c + i * 8 + 8]) for i in range(5)]
    bd_off, bd_len = segs[0]
    meta_off, _ = segs[1]
    seek_off, seek_len = segs[2]
    name_off, name_len = segs[3]
    data_off, _ = segs[4]
    b = d[bd_off:bd_off + bd_len]
    flags, count = struct.unpack('<II', b[0:8])
    metaSize, nameSize, align = struct.unpack('<III', b[8 + 64:8 + 64 + 12])
    # seek offset table (one dword per entry, 0xFFFFFFFF = none)
    if seek_len:
        seekhdr = struct.unpack('<%dI' % count, d[seek_off:seek_off + count * 4])
        seek_data_base = seek_off + count * 4
    else:
        seekhdr = [0xFFFFFFFF] * count
        seek_data_base = 0
    entries = []
    for i in range(count):
        e = d[meta_off + i * metaSize:meta_off + i * metaSize + metaSize]
        fad, fmt, po, pl, lo, ll = struct.unpack('<IIIIII', e[:24])
        codec = fmt & 0x3
        ch = (fmt >> 2) & 0x7
        rate = (fmt >> 5) & 0x3FFFF
        blk = (fmt >> 23) & 0xFF
        bps = (fmt >> 31) & 0x1
        if name_len:
            nm = d[name_off + i * nameSize:name_off + i * nameSize + nameSize]
            nm = nm.split(b'\0')[0].decode('ascii', 'replace')
        else:
            nm = "entry%02d" % i
        dpds = None
        if seekhdr[i] != 0xFFFFFFFF and seek_len:
            p = seek_data_base + seekhdr[i]
            n = struct.unpack('<I', d[p:p + 4])[0]
            dpds = struct.unpack('<%dI' % n, d[p + 4:p + 4 + 4 * n])
        entries.append(dict(idx=i, name=nm, codec=codec, ch=ch, rate=rate,
                            blk=blk, bps16=bps, po=data_off + po, pl=pl, dpds=dpds))
    return d, entries


def build_pcm_wav(d, e):
    raw = d[e['po']:e['po'] + e['pl']]
    ch = e['ch']; rate = e['rate']; bits = 16 if e['bps16'] else 8
    ba = ch * bits // 8; brate = rate * ba
    h = b'RIFF' + struct.pack('<I', 36 + len(raw)) + b'WAVE'
    h += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, ch, rate, brate, ba, bits)
    h += b'data' + struct.pack('<I', len(raw))
    return h + raw


def build_xwma(d, e):
    raw = d[e['po']:e['po'] + e['pl']]
    ch = e['ch']; rate = e['rate']
    nBlockAlign = aWMABlockAlign[e['blk'] & 0x1F]
    nAvgBytesPerSec = aWMAAvgBytesPerSec[(e['blk'] >> 5) & 0x7]
    wFormatTag = 0x0161  # WMAv2
    fmt = struct.pack('<HHIIHHH', wFormatTag, ch, rate, nAvgBytesPerSec,
                      nBlockAlign, 16, 0)
    dpds = e['dpds'] or ()
    dpds_bytes = struct.pack('<%dI' % len(dpds), *dpds)
    body = b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    if dpds:
        body += b'dpds' + struct.pack('<I', len(dpds_bytes)) + dpds_bytes
    body += b'data' + struct.pack('<I', len(raw)) + raw
    return b'RIFF' + struct.pack('<I', 4 + len(body)) + b'XWMA' + body


def build_msadpcm_wav(d, e):
    """Wrap MS-ADPCM payload in a RIFF/WAVE (wFormatTag 0x0002) for ffmpeg."""
    raw = d[e['po']:e['po'] + e['pl']]
    ch = e['ch']; rate = e['rate']
    # XACT ADPCM blockAlign field is (alignment/256 - 1) per channel adjustment;
    # the canonical MS-ADPCM nBlockAlign for XACT is (blk*32 + ... ) but the
    # robust value used by tools is: nBlockAlign = (blk + 22) * ch.
    # Most reliably, XACT stores adpcmBlockAlign = (blk * 2 + 32). We compute
    # nBlockAlign the way xactwb/towav do:
    nBlockAlign = (e['blk'] + 22) * ch
    wSamplesPerBlock = ((nBlockAlign - 7 * ch) * 8) // (4 * ch) + 2
    nAvgBytesPerSec = (rate // wSamplesPerBlock) * nBlockAlign
    # fmt chunk: WAVEFORMATEX(18) + cbSize body: wSamplesPerBlock(2) +
    # numCoef(2) + numCoef*(2+2)
    ncoef = len(MSADPCM_COEFS)
    cb = 2 + 2 + ncoef * 4
    fmt = struct.pack('<HHIIHH', 0x0002, ch, rate, nAvgBytesPerSec, nBlockAlign, 4)
    fmt += struct.pack('<H', cb)
    fmt += struct.pack('<HH', wSamplesPerBlock, ncoef)
    for c1, c2 in MSADPCM_COEFS:
        fmt += struct.pack('<hh', c1, c2)
    # 'fact' chunk recommended; ffmpeg tolerates its absence.
    body = b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    body += b'data' + struct.pack('<I', len(raw)) + raw
    return b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body


def emit(d, e, outdir):
    if e['codec'] == 0:
        data = build_pcm_wav(d, e); ext = '.wav'
    elif e['codec'] == 3:
        data = build_xwma(d, e); ext = '.xwma'
    elif e['codec'] == 2:
        data = build_msadpcm_wav(d, e); ext = '.wav'
    else:
        raise SystemExit("unsupported codec %d (%s)" % (e['codec'], CODEC[e['codec']]))
    p = os.path.join(outdir, e['name'] + ext)
    open(p, 'wb').write(data)
    return p


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('list', 'extract'):
        print("usage: lib_xwb.py list <xwb>\n"
              "       lib_xwb.py extract <xwb> <outdir> [name]", file=sys.stderr)
        sys.exit(2)
    cmd = sys.argv[1]; xwb = sys.argv[2]
    d, entries = parse(xwb)
    if cmd == 'list':
        print("# %s  entries=%d" % (xwb, len(entries)))
        for e in entries:
            print("%3d %-16s %-6s ch=%d rate=%d blk=%d len=%d" %
                  (e['idx'], e['name'], CODEC[e['codec']], e['ch'], e['rate'],
                   e['blk'], e['pl']))
        return
    outdir = sys.argv[3]; only = sys.argv[4] if len(sys.argv) > 4 else None
    os.makedirs(outdir, exist_ok=True)
    for e in entries:
        if only and e['name'] != only:
            continue
        p = emit(d, e, outdir)
        print(e['idx'], e['name'], CODEC[e['codec']], p)


if __name__ == '__main__':
    main()
