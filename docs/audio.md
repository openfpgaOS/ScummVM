# OpenFPGA Audio Subsystem

## 1. Architecture

Three-layer audio system: hardware PCM mixer, raw audio/FM output, and MIDI playback.

```
    CPU (VexRiscv @ 100 MHz)
         |
    ┌────┴───────────────────────────────────────────────────┐
    │                                                         │
    │  [of_mixer]  32-voice hardware PCM mixer                │
    │  ├─ CRAM1 sample memory (~11 MB)                        │
    │  ├─ Per-voice: addr, len, rate, vol_lr, loop, bidi      │
    │  ├─ 16.16 fixed-point resampling                        │
    │  ├─ 8-bit stereo volume with log curve + HW ramp        │
    │  ├─ Forward + bidirectional looping                     │
    │  └─ Voice-end IRQ bitmask                               │
    │         |                                               │
    │  [Hardware Mixer FSM]  ← zero CPU cost                  │
    │         |                                               │
    │         ▼                                               │
    │  [48 kHz Stereo Output]  ← I2S FIFO                    │
    │         |                                               │
    │  [of_audio / OPL3]  YMF262 FM synthesis (18 channels)   │
    │         |                                               │
    │         ▼                                               │
    │  [DAC / I2S]                                            │
    └─────────────────────────────────────────────────────────┘
```

### 1.1. MMIO Registers

| Register          | Offset | Access | Description                              |
|-------------------|--------|--------|------------------------------------------|
| MIX_VOICE_SEL     | 0xC0   | W      | Voice index 0-31                         |
| MIX_VOICE_ADDR    | 0xC4   | W      | CRAM1 word address                       |
| MIX_VOICE_LEN     | 0xC8   | W      | Sample length (sets LOOP_END default)    |
| MIX_VOICE_RATE    | 0xCC   | W      | Rate (16.16 fixed-point)                 |
| MIX_VOICE_CTRL    | 0xD0   | W      | [0]=active [1]=loop [2]=fmt16 [3]=bidi   |
| MIX_VOICE_POS     | 0xD0   | R      | Current position[21:0]                   |
| MIX_CTRL          | 0xD4   | RW     | [0]=enable                               |
| MIX_VOICE_VOL_LR  | 0xD8   | W      | Current volume {R[15:8], L[7:0]}         |
| MIX_VOICE_LOOP_END| 0xE4   | W      | Loop end point[21:0]                     |
| MIX_VOICE_POS_WR  | 0xE8   | W      | Set position[21:0]                       |
| MIX_VOICE_LOOP_START| 0xEC | W      | Loop start point[21:0]                   |
| MIX_VOICE_VOL_TARGET| 0xF0 | W      | Target volume {R[7:0], L[7:0]}           |
| MIX_VOICE_VOL_RATE| 0xF4   | W      | Ramp step (0=instant)                    |
| MIX_IRQ_PENDING   | 0xF8   | R      | Voice-end bitmask                        |
| MIX_IRQ_CLEAR     | 0xF8   | W      | W1C clear                                |

### 1.2. API Layers

| Layer       | Header        | Purpose                                 |
|-------------|---------------|------------------------------------------|
| of_mixer    | of_mixer.h    | 32-voice hardware PCM playback           |
| of_audio    | of_audio.h    | Raw PCM streaming + OPL3 FM register I/O |
| of_midi     | of_midi.h     | MIDI file playback over OPL3             |
| of_codec    | of_codec.h    | WAV/VOC format parsing                   |
| SDL_mixer   | SDL_mixer.h   | SDL_mixer shim wrapping of_mixer for ports |

---

## 2. Comparison to Era Hardware

### 2.1. vs. Amiga Paula (1985)

Paula is the closest ancestor. The mixer is essentially "Paula done right."

| Feature        | Amiga Paula                           | of_mixer                          |
|----------------|---------------------------------------|-----------------------------------|
| Voices         | 4 (HW DMA)                           | 32 (HW DMA)                      |
| Sample format  | 8-bit signed                          | 16-bit signed (+ 8-bit)          |
| Volume         | 6-bit (0-64)                          | 8-bit (0-255) + log curve        |
| Stereo         | Hard L/R (0,3=left; 1,2=right)        | Per-voice 8-bit L/R              |
| Resampling     | Period-based (3.58MHz / period)        | 16.16 fixed-point ratio          |
| Looping        | Forward only, full sample              | Forward + bidi, start/end points |
| Position read  | No                                    | Yes                              |
| Volume ramp    | No (clicks on changes)                | HW ramp (configurable rate)      |
| IRQ            | Per-voice end                         | Per-voice end bitmask            |
| CPU cost       | Zero (DMA)                            | Zero (FPGA fabric)               |

Key improvements over Paula: volume ramping prevents clicks, `retrigger` updates ADDR/LEN/POS atomically (required careful Paula programming to avoid glitches), and `set_voice_raw` batches rate+volume like MOD tracker replayers need.

### 2.2. vs. Sound Blaster 16 (1992)

| Feature        | SB16                                  | of_mixer + of_audio               |
|----------------|---------------------------------------|-----------------------------------|
| PCM playback   | 1 voice, DMA to ISA bus               | 32 voices, hardware mixed         |
| Sample rate    | Up to 44.1 kHz                        | 48 kHz output, arbitrary input    |
| Sample format  | 8 or 16-bit, mono/stereo              | 16-bit signed mono per voice      |
| Mixing         | Software (CPU mixes all voices)       | Hardware (zero CPU)               |
| FM synthesis   | OPL3 (YMF262)                         | OPL3 (YMF262) — identical         |
| MIDI           | MPU-401 UART                          | Software player over OPL3         |
| DMA            | ISA DMA channels                      | CRAM1 memory-mapped               |

The SB16 was a 1-voice device. Doom mixed all SFX in software into a single DMA buffer. Hardware mixing eliminates that CPU cost entirely — critical at 100 MHz RISC-V.

OPL3 register interface is identical to the SB16 port I/O model: `of_audio_opl_write(reg, val)` maps directly to `outb(0x388, reg); outb(0x389, val)`, making Doom/Duke3D FM music nearly drop-in.

### 2.3. vs. Gravis Ultrasound (GUS, 1992)

The GUS is the **closest architectural match**:

| Feature        | GUS                                   | of_mixer                          |
|----------------|---------------------------------------|-----------------------------------|
| Voices         | 14-32 (more = lower sample rate)      | 32 (fixed 48 kHz)                |
| Sample memory  | 256 KB-1 MB onboard DRAM              | CRAM1 pool (~11 MB)              |
| Sample format  | 8 or 16-bit unsigned                  | 16-bit signed (+ 8-bit)          |
| Volume         | 12-bit log (4096 levels)              | 8-bit log (256 levels)           |
| Stereo         | 4-bit pan (16 positions)              | 8-bit per-channel (256 pos)      |
| Resampling     | 14-bit frequency counter              | 16.16 fixed-point                |
| Looping        | Forward, bidi, with start/end         | Forward, bidi, with start/end    |
| Position read  | Yes (current address)                 | Yes                              |
| Volume ramp    | HW ramp with programmable rate        | HW ramp with programmable rate   |
| IRQ            | Voice end, vol ramp end, wavetable    | Voice end bitmask only           |
| Memory alloc   | App manages GUS DRAM manually         | Bump allocator                   |
| CPU cost       | Zero (HW mixing)                      | Zero (FPGA fabric)               |

Key differences:

- GUS voice count affected sample rate (32 voices = 19.2 kHz each). The FPGA runs all 32 at 48 kHz.
- GUS had separate DRAM with explicit upload. CRAM1 is CPU-writable directly.
- GUS had volume ramp end IRQ — useful for tracker envelopes and fade-outs.
- GUS lacked OPL3, hurting DOS game compatibility. of_mixer + OPL3 covers both.

### 2.4. vs. DirectSound / AC97 (mid-90s)

| Feature        | DirectSound (Win95+)                  | of_mixer                          |
|----------------|---------------------------------------|-----------------------------------|
| Voices         | Unlimited (software mixed)            | 32 (hardware)                     |
| Mixing         | CPU (MMX-optimized)                   | Zero CPU                          |
| Latency        | 20-100 ms (buffer-based)              | ~1 sample (~21 μs)               |
| 3D audio       | DS3D / EAX                            | of_mixer_3d (CPU-side)            |
| Streaming      | Circular buffer with write cursors    | of_audio_stream (ping-pong voices)|

The industry moved to software mixing because CPUs got fast enough. At 100 MHz RISC-V, hardware mixing is the correct choice.

---

## 3. Bugs Fixed

- [x] `of_mixer_set_pan`: added vol/pan shadow — center pan now outputs full volume
- [x] `SDL_mixer.h Mix_LoadWAV`: samples now kept as 16-bit signed in CRAM1
- [x] `SDL_mixer.h Mix_PlayChannel`: `loops` parameter now calls `of_mixer_set_loop`
- [x] `of_audio_write`: ping-pong scratch buffers prevent overwriting playing data
- [x] `of_audio_write`: accepts mono input directly (no forced stereo conversion)
- [x] `of_mixer_play`: priority-based voice stealing when all voices are busy

---

## 4. Feature Roadmap

### Phase 1: Bug Fixes — `[x]` done

### Phase 2: 8-bit Sample Support — `[x]` done

- [x] `of_mixer_play_8bit(pcm_s8, count, rate, priority, vol)` — plays 8-bit signed mono
- [x] SDL_mixer shim dispatches to 8-bit or 16-bit path based on WAV source format
- [x] Services table extended (ABI-safe append)

**Files changed:** `mixer.c`, `mixer.h`, `of_services.h`, `of_mixer.h`, `SDL_mixer.h`

### Phase 3: Group / Master Volume — `[x]` done

- [x] 4 groups: `OF_MIXER_GROUP_SFX` (0), `MUSIC` (1), `VOICE` (2), `AUX` (3)
- [x] `of_mixer_set_group(voice, group)` — assign voice to group
- [x] `of_mixer_set_group_volume(group, vol)` — scale all voices in group
- [x] `of_mixer_set_master_volume(vol)` — scale everything
- [x] Volume chain: voice × group × master, applied through `apply_vol_pan()`

**Files changed:** `mixer.c`, `mixer.h`, `of_services.h`, `of_mixer.h`, `services_table.c`

### Phase 4: 3D Spatialization — `[x]` done

New header: `of_mixer_3d.h` (CPU-side, no OS changes needed).

- [x] `of_mixer_3d_set_listener(x, y, angle)` — listener position (16.16 FP)
- [x] `of_mixer_3d_set_source(voice, x, y)` — source position
- [x] `of_mixer_3d_set_source_dist(voice, min, max)` — per-source distance range
- [x] `of_mixer_3d_set_attenuation(model, ref_dist, max_dist)` — global model
- [x] `of_mixer_3d_update()` — recompute all volumes/pans (call once per frame)
- [x] 3 attenuation models: linear, inverse, inverse-clamped
- [x] Angle-to-pan via rotated dot product (cosine panning)

**Files added:** `of_mixer_3d.h`

### Phase 5: Streaming Audio — `[x]` done

Ping-pong double-buffered streaming using mixer voices 29+30.

- [x] `of_audio_stream_open(sample_rate)` — open stream (resampled to 48kHz)
- [x] `of_audio_stream_write(samples, count)` — write up to 32K mono samples
- [x] `of_audio_stream_ready()` — non-blocking check if next buffer is available
- [x] `of_audio_stream_close()` — stop and release voices

**Files changed:** `audio.c`, `audio.h`, `of_services.h`, `of_audio.h`, `services_table.c`

### Phase 6: Volume Ramp End IRQ — `[ ]`

IRQ when a voice's volume ramp reaches its target. Enables "fade to zero then stop",
tracker envelopes, and crossfades.

**Cost:** ~50 ALMs, 0 M10K. Low risk (passive observation of existing state).

#### 6.1. RTL: Add ramp-end detection — `audio_mixer.v`

The ramp logic in `S_VOL_RAMP` (lines 539-562) already computes `new_l`/`new_r`
and compares against targets. Add transition detection:

- [ ] Add `vol_ramp_pending[31:0]` output register (alongside `voice_end_pending`)
- [ ] Add `vol_ramp_irq` output wire: `assign vol_ramp_irq = |vol_ramp_pending`
- [ ] In `S_VOL_RAMP`, after computing `new_l`/`new_r` in the `ramp_block`:
  detect the **transition** (was not at target, now is):
  ```
  if (new_l == cur_target_l && new_r == cur_target_r
      && (cur_vol_l != cur_target_l || cur_vol_r != cur_target_r))
      vol_ramp_pending[cur_voice] <= 1;
  ```
- [ ] Add W1C clear logic (same pattern as `irq_clear_wr` for voice-end at line 316):
  ```
  if (ramp_irq_clear_wr)
      vol_ramp_pending <= vol_ramp_pending & ~ramp_irq_clear;
  ```
- [ ] Reset `vol_ramp_pending` to 0 in the reset block (line 306)

**ALM breakdown:** 32 FFs for pending register (~32), W1C clear (~10),
transition detect: two 8-bit comparators + AND (~8). Total: ~50.

#### 6.2. Register interface — `axi_periph_slave.v`

- [ ] Map `vol_ramp_pending` at offset 0xFC (read = pending bitmask, write = W1C clear)
- [ ] Wire `ramp_irq_clear` and `ramp_irq_clear_wr` signals from AXI decode
- [ ] Follow the exact pattern of 0xF8 (`voice_end_pending`/`irq_clear`)

#### 6.3. Top-level wiring — `core_top.v`

- [ ] Connect `vol_ramp_pending`, `vol_ramp_irq`, `ramp_irq_clear`, `ramp_irq_clear_wr`
      between `audio_mixer` and `axi_periph_slave`
- [ ] Optionally OR `vol_ramp_irq` into the CPU IRQ line (if IRQ-driven mixer is desired later)

#### 6.4. MMIO register definition — `regs.h`

- [ ] Add `MIX_VOL_RAMP_PENDING  REG32(SYSREG_BASE + 0xFC)` (read: bitmask, write: W1C)

#### 6.5. Firmware API — `mixer.c` / `mixer.h`

- [ ] `of_mixer_poll_ramp_ended()` — read `MIX_VOL_RAMP_PENDING`, clear bits, return mask
- [ ] `of_mixer_set_ramp_callback(void (*cb)(uint32_t mask))` — optional callback
      (same pattern as `of_mixer_set_end_callback`)

#### 6.6. SDK API — `of_services.h` / `of_mixer.h`

- [ ] Append `mixer_poll_ramp_ended` and `mixer_set_ramp_callback` to services table
- [ ] Add inline wrappers in `of_mixer.h` (FPGA) and no-op stubs (PC)

#### 6.7. Verification

- [ ] Verilator: play a voice, set target to 0 with rate=1, confirm pending bit fires
      once when vol reaches 0 — not before, not repeatedly
- [ ] Firmware: fade a voice to 0, poll for ramp end, stop voice on callback.
      Confirm no click (volume is already 0 when stopped)

---

### Phase 7: Reverb / Echo — `[ ]`

Single-tap delay line in CRAM1, piggybacked on the mixer FSM as an extra processing
pass after voice 31. The round-robin already has ~900 cycles of headroom per sample;
the reverb pass uses ~36 of those.

**Cost:** ~120 ALMs, 0 M10K. Medium risk (new CRAM1 write path).

**Limitation:** Single-tap only (no multi-tap / chorus). Upgrade to dedicated AXI
port (~300 ALMs) if multi-tap is needed later.

#### 7.1. RTL: Add CRAM1 write port — `audio_mixer.v`

The mixer currently only reads CRAM1 (lines 40-44). Reverb needs to write the
feedback sample back to the delay buffer.

- [ ] Add output signals to module port list:
  ```
  output reg         cram1_wr,
  output reg  [31:0] cram1_wdata,
  ```
- [ ] Reset `cram1_wr` to 0 on reset and at top of always block (same as `cram1_rd`)

#### 7.2. RTL: Add reverb configuration registers — `audio_mixer.v`

New input ports from `axi_periph_slave`:

- [ ] `reverb_enable` (1 bit) — global on/off
- [ ] `reverb_delay_base` (22 bits) — CRAM1 word address of delay buffer start
- [ ] `reverb_delay_len` (22 bits) — delay buffer length in samples
- [ ] `reverb_feedback` (8 bits) — feedback coefficient (0=none, 255=infinite)
- [ ] `reverb_wet` (8 bits) — wet mix level (0=dry only, 255=full wet)
- [ ] `reverb_send_global` (8 bits) — global send level (scales the dry mix fed into delay)

Internal state:

- [ ] `reverb_wr_ptr` (22 bits) — circular write pointer into delay buffer
- [ ] `reverb_rd_ptr` (22 bits) — read pointer = wr_ptr (read and write same position: overwrite)
- [ ] `reverb_send_accum` (signed 32 bits) — accumulated send signal from voice mix

#### 7.3. RTL: Accumulate send during voice mix — `audio_mixer.v`

During the existing `S_ACCUM` state (line 530), add a parallel send accumulation.
Uses a fixed global send level (per-voice send deferred to a future iteration to
avoid adding a BRAM read per voice):

- [ ] In `S_IDLE` when starting a new mix cycle, reset: `reverb_send_accum <= 0`
- [ ] In `S_ACCUM`, add:
  ```
  reverb_send_accum <= reverb_send_accum
      + {{16{pipe_scaled_l[15]}}, pipe_scaled_l};
  ```
  (This accumulates the mono mix of all voices into the send bus.)

#### 7.4. RTL: Add reverb FSM states — `audio_mixer.v`

Insert 5 new states between voice 31's final writeback and `S_OUTPUT`. Modify
the transitions at lines 402, 616, 630 where `cur_voice == 5'd31` currently
goes to `S_OUTPUT`:

```
if (cur_voice == 5'd31) begin
    if (reverb_enable)
        state <= S_REVERB_REQ;
    else
        state <= S_OUTPUT;
end
```

New states:

- [ ] `S_REVERB_REQ` — Issue CRAM1 read for delayed sample:
  ```
  cram1_rd <= 1;
  cram1_addr <= reverb_delay_base + reverb_rd_ptr;
  state <= S_REVERB_WAIT;
  ```

- [ ] `S_REVERB_WAIT` — Wait for CRAM1 data valid:
  ```
  if (cram1_rdata_valid) begin
      reverb_delayed <= $signed(cram1_rdata[15:0]);  // 16-bit signed delayed sample
      state <= S_REVERB_MIX;
  end
  ```

- [ ] `S_REVERB_MIX` — Compute wet mix and feedback, add wet to accumulators:
  ```
  // Wet: delayed sample × wet level → add to output
  wire signed [23:0] wet_prod = reverb_delayed * $signed({1'b0, reverb_wet});
  accum_l <= accum_l + {{8{wet_prod[23]}}, wet_prod};
  accum_r <= accum_r + {{8{wet_prod[23]}}, wet_prod};

  // Feedback: (send × send_level + delayed × feedback) → write back
  wire signed [23:0] send_scaled = reverb_send_accum[31:16] * $signed({1'b0, reverb_send_global});
  wire signed [23:0] fb_prod = reverb_delayed * $signed({1'b0, reverb_feedback});
  reverb_writeback <= send_scaled[23:8] + fb_prod[23:8];  // 16-bit signed

  state <= S_REVERB_WR;
  ```
  Reuses the existing combinational multiplier nets — `prod_l`/`prod_r` are
  idle at this point since voice processing is done. May need dedicated multiply
  wires if timing is tight.

- [ ] `S_REVERB_WR` — Write feedback sample to delay buffer:
  ```
  cram1_wr <= 1;
  cram1_addr <= reverb_delay_base + reverb_wr_ptr;
  cram1_wdata <= {{16{reverb_writeback[15]}}, reverb_writeback};
  state <= S_REVERB_ADV;
  ```

- [ ] `S_REVERB_ADV` — Advance circular pointer:
  ```
  if (reverb_wr_ptr + 1 >= reverb_delay_len)
      reverb_wr_ptr <= 0;
  else
      reverb_wr_ptr <= reverb_wr_ptr + 1;
  state <= S_OUTPUT;
  ```

**ALM breakdown:** 5 states decode (~30), pointers + config regs (~15),
send accumulator (~20), feedback multiply (~20), write path control (~15).
Multiply reuse from existing `prod_l`/`prod_r` saves ~20 ALMs vs dedicated.
Total: ~120.

#### 7.5. RTL: Wire CRAM1 write through CDC — `core_top.v`

The CRAM1 CDC arbiter in `core_top.v` (lines ~2600-2630) currently handles
CPU reads/writes + mixer reads. Add mixer writes:

- [ ] Connect `cram1_wr` and `cram1_wdata` from `audio_mixer` to the CDC arbiter
- [ ] The arbiter already handles write strobes from the CPU — add a mux:
  if mixer write is asserted and CPU is not writing, route mixer write through.
  Mixer writes are 1/sample (48K/sec) — negligible contention.
- [ ] Ensure `cram1_wr` is deasserted after one clock cycle (same as `cram1_rd`)

#### 7.6. Register interface — `axi_periph_slave.v`

Map reverb configuration to new MMIO offsets (e.g., 0x100-0x114):

- [ ] 0x100: `REVERB_CTRL` — [0]=enable, [15:8]=wet, [23:16]=feedback, [31:24]=send
- [ ] 0x104: `REVERB_DELAY_BASE` — CRAM1 word address of delay buffer
- [ ] 0x108: `REVERB_DELAY_LEN` — delay length in samples
- [ ] 0x10C: `REVERB_STATUS` — [21:0]=current write pointer (debug/readback)

#### 7.7. MMIO register definitions — `regs.h`

- [ ] Add register defines for 0x100-0x10C

#### 7.8. Firmware: delay buffer allocation — `mixer.c`

- [ ] Allocate delay buffer from CRAM1 bump allocator at init time:
  ```
  #define REVERB_MAX_MS    500
  #define REVERB_MAX_SAMPLES (48 * REVERB_MAX_MS)  // 24000 samples = 48KB
  ```
- [ ] `of_mixer_reverb_init()` — allocate CRAM1 buffer, zero it, set base/len registers
- [ ] `of_mixer_set_reverb(delay_ms, feedback, wet)` — compute sample count from ms,
  write REVERB_CTRL + REVERB_DELAY_LEN registers
- [ ] `of_mixer_reverb_disable()` — write enable=0 to REVERB_CTRL

#### 7.9. Firmware: send level API — `mixer.c` / `mixer.h`

- [ ] `of_mixer_set_reverb_send(level)` — set global send level (0-255)
- [ ] Write to REVERB_CTRL[31:24]

#### 7.10. SDK API — `of_services.h` / `of_mixer.h`

- [ ] Append to services table: `mixer_set_reverb`, `mixer_set_reverb_send`
- [ ] Add inline wrappers in `of_mixer.h`
- [ ] PC stubs (no-op)

#### 7.11. Verification — Verilator

Simulate before hardware. Key test cases:

- [ ] **Basic echo:** play a click sample, verify delayed copy appears in output
      at exactly `delay_ms` later
- [ ] **Feedback decay:** set feedback=128, verify echo repeats with halving amplitude
- [ ] **No reverb:** set enable=0, verify output is identical to pre-reverb baseline
- [ ] **Pointer wrap:** use a short delay buffer (64 samples), run for 10+ wraps,
      verify no glitches at wrap boundary
- [ ] **CRAM1 contention:** play 32 voices + reverb simultaneously, verify no
      sample corruption or FIFO underrun
- [ ] **CPU contention:** CPU reads/writes CRAM1 while reverb is active, verify
      no corruption in delay buffer or voice data

#### 7.12. Verification — Hardware

- [ ] Simple test: play a clap sample through reverb with 200ms delay,
      feedback=100, wet=128. Listen for clean echo without clicks or noise.
- [ ] Stress test: Duke3D with reverb enabled in a large room.
      Monitor `active_count` and FIFO level for underruns.

---

## 5. Per-Game Readiness

| Game       | Ph1-5 | Ph6 Ramp | Ph7 Reverb | Notes                            |
|------------|-------|----------|------------|-----------------------------------|
| Doom       | done  |          |            | All audio features covered        |
| Duke3D     | done  |          | nice       | Room echo for large areas         |
| Quake      | done  |          | nice       | Per-room reverb presets           |
| Quake 2    | done  |          | nice       | Same as Quake                     |
| Half-Life  | done  |          | nice       | Environment reverb zones          |
| SM64       | done  |          |            | No reverb in original             |
| Descent    | done  |          | nice       | Tunnel echo                       |
| Wipeout    | done  |          |            | No reverb in original             |

All games have their required audio features. Phases 6-7 are quality improvements.

---

## 6. Cost Summary

| Phase | ALMs | M10K | Status | Enables                          |
|-------|------|------|--------|----------------------------------|
| Ph1   | 0    | 0    | done   | All games (correctness)          |
| Ph2   | 0    | 0    | done   | Doom, Duke3D (native 8-bit SFX)  |
| Ph3   | 0    | 0    | done   | Quake+, Wipeout (volume control) |
| Ph4   | 0    | 0    | done   | Quake, Descent, HL (3D audio)    |
| Ph5   | 0    | 0    | done   | Wipeout, HL (streaming music)    |
| Ph6   | ~50  | 0    | todo   | Tracker envelopes, fade-to-stop  |
| Ph7   | ~120 | 0    | todo   | Duke3D/Quake room effects        |

Phases 1-5 complete — all software, zero FPGA impact.
Phases 6-7 total: ~170 ALMs, 0 M10K.

### Files affected by Phase 6

| File | Repo | Changes |
|------|------|---------|
| `audio_mixer.v` | openfpgaOS | Add `vol_ramp_pending` register + transition detect |
| `axi_periph_slave.v` | openfpgaOS | Map 0xFC read/write |
| `core_top.v` | openfpgaOS | Wire new signals |
| `regs.h` | openfpgaOS | Add `MIX_VOL_RAMP_PENDING` define |
| `mixer.c` | openfpgaOS | `poll_ramp_ended`, callback support |
| `mixer.h` | openfpgaOS | Declarations |
| `of_services.h` | both repos | Append to services table |
| `of_mixer.h` | SDK | Inline wrappers + PC stubs |
| `services_table.c` | openfpgaOS | Wire new functions |

### Files affected by Phase 7

| File | Repo | Changes |
|------|------|---------|
| `audio_mixer.v` | openfpgaOS | CRAM1 write port, 5 FSM states, reverb config regs, send accumulator |
| `axi_periph_slave.v` | openfpgaOS | Map 0x100-0x10C registers |
| `core_top.v` | openfpgaOS | Wire CRAM1 write through CDC arbiter |
| `regs.h` | openfpgaOS | Reverb register defines |
| `mixer.c` | openfpgaOS | Delay buffer alloc, reverb config API |
| `mixer.h` | openfpgaOS | Declarations + group defines |
| `of_services.h` | both repos | Append to services table |
| `of_mixer.h` | SDK | Inline wrappers + PC stubs |
| `services_table.c` | openfpgaOS | Wire new functions |

### Execution order

Phase 6 should be done first — it's a quick win (~50 ALMs, low risk) and the
"fade to zero then stop" pattern is useful for clean reverb tail shutdown in Phase 7.

Within Phase 7, the order should be:

1. CRAM1 write port plumbing (7.1, 7.5) — the risky part, do first
2. Verilator simulation of write path (7.11 contention tests)
3. Reverb FSM states (7.3, 7.4)
4. Register interface (7.6, 7.7)
5. Firmware API (7.8, 7.9, 7.10)
6. Full simulation (7.11 all tests)
7. Hardware validation (7.12)
