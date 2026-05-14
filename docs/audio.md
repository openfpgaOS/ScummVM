# openfpgaOS Audio Subsystem

## Current Architecture

The SDK audio stack is centered on the hardware PCM mixer:

```
App / MIDI engine
    |
    | 16-bit mono samples, .ofsf banks, or streaming PCM
    v
SDRAM sample pool / audio ring
    |
    | FPGA AXI reads
    v
32-voice hardware PCM mixer
    |
    | fixed 48 kHz stereo
    v
Audio output FIFO / DAC
```

The older CRAM1/software-mixer descriptions are obsolete. Current firmware
keeps mixer samples in an 8 MB SDRAM sample pool. CPU writes are flushed or
sent through the uncached SDRAM alias before the hardware mixer reads them.

## API Layers

| Layer | Header | Purpose |
|---|---|---|
| `of_mixer` | `of_mixer.h` | 32-voice hardware PCM sample playback |
| `of_audio` | `of_audio.h` | Raw 48 kHz stereo writes and mono stream helper |
| `of_midi` | `of_midi.h` | Standard MIDI playback through `.ofsf` sample banks |
| `of_codec` | `of_codec.h` | WAV/VOC parsing |
| SDL shims | `SDL.h`, `SDL_mixer.h` | Compatibility wrappers for ports |

## Mixer Contract

`of_mixer` is an autonomous FPGA mixer. Apps do not pump audio; `of_mixer_pump()`
is a compatibility no-op.

Key properties:

- 32 hardware voices.
- Fixed 48 kHz stereo output.
- Native sample format is signed 16-bit mono PCM.
- `of_mixer_play_8bit()` accepts signed 8-bit mono and expands to 16-bit.
- Samples must come from `of_mixer_alloc_samples()` or a preloaded `.ofsf`
  bank so their addresses live inside the SDRAM sample pool.
- Per-voice rate uses 16.16 fixed-point resampling with linear interpolation.
- Volume is 0-255, with per-voice pan or direct left/right volume.
- Forward loops are supported through `of_mixer_set_loop()`.
- Voice-end state is exposed through `of_mixer_poll_ended()`.
- Group and master volume are handled in hardware.

Compatibility surfaces:

- `of_mixer_set_bidi()` is retained for older callers, but current hardware
  ignores bidirectional looping.
- `of_mixer_set_filter()` is retained for older callers, but current hardware
  does not implement per-voice filtering.

Minimal sample playback:

```c
of_mixer_init(32, OF_MIXER_OUTPUT_RATE);

int16_t *pcm = of_mixer_alloc_samples(sample_count * sizeof(int16_t));
memcpy(pcm, decoded_mono_s16, sample_count * sizeof(int16_t));

int voice = of_mixer_play((const uint8_t *)pcm,
                          sample_count,
                          source_rate_hz,
                          0,
                          255);
of_mixer_set_pan(voice, 128);
```

## Raw PCM

`of_audio_write()` queues interleaved signed 16-bit stereo pairs:

```c
of_audio_init();
int free_pairs = of_audio_free();
int written = of_audio_write(stereo_pairs, count);
```

For longer mono streams, use the stream helper:

```c
of_audio_stream_open(sample_rate_hz);
while (more_audio) {
    if (of_audio_stream_ready())
        of_audio_stream_write(mono_s16, count);
}
of_audio_stream_close();
```

The stream helper uses reserved mixer resources and the SDRAM audio ring.

## MIDI

MIDI playback is sample-based, not OPL/FM. Ship a `.ofsf` SoundFont bank in a
data slot; the kernel preloads the first bank it finds into the mixer sample
pool and exposes it through the services table.

```c
of_midi_init();
of_midi_play(midi_data, midi_len, 1);
```

`of_midi_play()` installs the MIDI pump on the timer ISR. Do not call
`of_midi_pump()` from the main loop while playback is active.

## Era Hardware Comparison

| Feature | Gravis Ultrasound | openfpgaOS mixer |
|---|---|---|
| Model | Hardware wavetable/sample playback | Hardware PCM sample playback |
| Voices | Up to 32 hardware voices | 32 hardware voices |
| Sample memory | On-card DRAM | SDRAM sample pool |
| Output | Voice-count-dependent quality/rate on classic cards | Fixed 48 kHz stereo |
| Compatibility | GUS registers and DOS drivers | SDK API only, not GUS-compatible |

The mixer is GUS-like architecturally: many hardware PCM voices reading sample
memory. It is not a GUS emulation layer. Existing software would need an
explicit compatibility layer for GF1 registers, patch memory behavior, IRQs,
and driver expectations.

Compared with Sound Blaster-style PCM, the important difference is that apps do
not mix every sound into one DMA buffer on the CPU. The FPGA mixer reads each
voice from SDRAM and produces the final 48 kHz stereo stream.

## Slot Notes

Audio assets usually live in app data slots:

- General data: slots 3-6.
- Optional `.ofsf` SoundFont bank: slot 7, or any data slot containing the
  first `.ofsf` the kernel discovers.
- Slot 8 is SDK/system shared config and must not be used for app audio data.
