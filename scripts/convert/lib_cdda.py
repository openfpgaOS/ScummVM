#!/usr/bin/env python3
"""ScummVM CDDA.SOU codec (encode/decode) -- pure-python + numpy.

CDDA.SOU is the single-file CD-audio container ScummVM reads for Loom CD
(engines/scumm/cdda.cpp). Format:

  - an 800-byte header (START_OF_CDDA_DATA), ignored by the decoder
  - then fixed 1177-byte blocks (BLOCK_SIZE), each:
      byte 0 : per-block shift -- high nibble = left shift, low nibble = right
      bytes 1..1176 : 588 stereo frames as (int8 L, int8 R) pairs

  The decoder reconstructs each 16-bit sample as `int8 << shift`. One Red-Book
  CD audio sector (2352 bytes = 588 stereo 16-bit frames) maps to exactly one
  block, so a raw audio rip transcodes 1:1.

This module is engine-agnostic: it transcodes raw 16-bit-LE stereo PCM <-> the
block format. Where the audio starts on a disc (data track + pregap) is the
caller's job -- see convert/build_loom_iso.py for the Loom framing.

API:
  encode(src, out, start_byte, n_sectors, chunk_sectors=20000, progress=None)
  decode_block(block_bytes) -> (numpy int16 left[588], int16 right[588])
"""
import numpy as np

HDR_SIZE = 800        # START_OF_CDDA_DATA
BLOCK_SIZE = 1177     # 1 shift byte + 1176 sample bytes
SECTOR_BYTES = 2352   # one CD audio sector == one block
FRAMES = 588          # stereo frames per block
NSAMP = 1176          # int16 samples per block (FRAMES * 2)
MAX_SHIFT = 15        # shift is a nibble; int16 input never needs > 9


def _per_block_shift(channel):
    """Smallest per-row shift s with (max|channel| >> s) <= 127, capped at
    MAX_SHIFT. `channel` is an int32 array shaped (nblocks, FRAMES)."""
    peak = np.max(np.abs(channel), axis=1)        # int32, max 32768
    shift = np.zeros(peak.shape, dtype=np.int32)
    for _ in range(MAX_SHIFT):                     # int16 input converges by 9
        need = (peak >> shift) > 127
        if not need.any():
            break
        shift[need] += 1
    return shift


def _encode_chunk(buf):
    """Transcode a whole number of 2352-byte sectors (bytes) -> CDDA.SOU
    blocks (bytes). buf length must be a multiple of SECTOR_BYTES."""
    nblocks = len(buf) // SECTOR_BYTES
    audio = np.frombuffer(buf[:nblocks * SECTOR_BYTES], dtype='<i2').reshape(nblocks, NSAMP)
    left = audio[:, 0::2].astype(np.int32)         # cast first: abs(-32768) is safe
    right = audio[:, 1::2].astype(np.int32)

    sl = _per_block_shift(left)
    sr = _per_block_shift(right)
    l8 = np.clip(np.right_shift(left, sl[:, None]), -128, 127).astype(np.int8)
    r8 = np.clip(np.right_shift(right, sr[:, None]), -128, 127).astype(np.int8)

    block = np.empty((nblocks, BLOCK_SIZE), dtype=np.uint8)
    block[:, 0] = ((sl << 4) | sr).astype(np.uint8)
    inter = np.empty((nblocks, NSAMP), dtype=np.int8)
    inter[:, 0::2] = l8
    inter[:, 1::2] = r8
    block[:, 1:] = inter.view(np.uint8)
    return block.tobytes()


def encode(src, out, start_byte, n_sectors, chunk_sectors=20000, progress=None):
    """Transcode `n_sectors` of raw 16-bit-LE stereo PCM (2352 B/sector) from
    file-like `src` (read from `start_byte`) into a CDDA.SOU on file-like `out`.

    Returns the number of blocks written. `progress(done, total)` is called per
    chunk if given. Memory stays bounded (one chunk at a time)."""
    out.write(b'\x00' * HDR_SIZE)
    src.seek(start_byte)
    done = 0
    while done < n_sectors:
        n = min(chunk_sectors, n_sectors - done)
        buf = src.read(n * SECTOR_BYTES)
        got = len(buf) // SECTOR_BYTES
        if got == 0:
            break
        out.write(_encode_chunk(buf[:got * SECTOR_BYTES]))
        done += got
        if progress:
            progress(done, n_sectors)
        if got < n:                                # short read = EOF
            break
    return done


def decode_block(block):
    """Decode one 1177-byte block -> (int16 left[588], int16 right[588]).
    Mirrors cdda.cpp::readBuffer; used for round-trip verification."""
    shift = block[0]
    sl, sr = shift >> 4, shift & 0x0F
    samples = np.frombuffer(block[1:1 + NSAMP], dtype=np.int8).astype(np.int32)
    left = (samples[0::2] << sl).astype(np.int16)
    right = (samples[1::2] << sr).astype(np.int16)
    return left, right
