# wmapro_test — PC unit test for the WMA Pro decoder

Two small native tools used to validate `audio/decoders/wmapro.cpp`
against FFmpeg's reference implementation.

## What's here

* `test.cpp` — links our `WMAProCodec` against the host's libstdc++,
  parses an XACT XWB wave bank, decodes one entry, writes PCM to disk.
* `wrap_xwma.cpp` — wraps raw xWMA bytes from an XWB entry as a
  RIFF/XWMA container that `ffmpeg` can ingest, so we can produce a
  reference PCM stream from the same bytes.
* `stubs.cpp` — minimal stubs for ScummVM externs the wider
  audio/common library references but our test path doesn't exercise
  (Mutex, debug(), Common::File, etc.).

## Build

```sh
make
```

Produces `wmapro_test` and `wrap_xwma`.

## Use

```sh
# Decode entry 0 with our decoder
./wmapro_test ~/MusicOriginal.xwb 0 ours.raw

# Reference via ffmpeg
./wrap_xwma /tmp/entry0.bin /tmp/entry0.xwm 44100 2 4459 96000
ffmpeg -v warning -i /tmp/entry0.xwm -f s16le -y ref.raw
```

`ours.raw` and `ref.raw` are 16-bit little-endian PCM, stereo @ 44.1 kHz,
ready to `cmp` byte-by-byte or play via `ffplay -f s16le -ar 44100 -ac 2`.

## What this turned up (May 2026)

The MI1 SE music wave bank entries are encoded with WMA Pro features
mainline FFmpeg's `wmaprodec.c` flags as **unsupported**:

```
[wmapro] Reserved bit is not implemented.
[wmapro] Channel transform bit is not implemented.
```

FFmpeg decodes ≈10–16% of each entry's expected sample count before
bailing.  Our port (which is a translation of FFmpeg's wmaprodec.c)
inherits the same limitation.

The block_align is reverse-engineered as **4459 bytes per packet**
for XACT XWB entries with `align==6`, confirmed by every entry length
dividing evenly by 4459.

See `src/scummvm/scummvm/audio/decoders/wmapro.cpp` for the decoder
itself.  Re-enabling the SoundSE call into it for MI1 SE is gated
because the resulting partial decode caused engine-side state issues
(see the parent commit history).
