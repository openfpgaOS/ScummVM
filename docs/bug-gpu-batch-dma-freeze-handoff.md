# Handoff prompt — GPU doorbell-DMA deadlock

## Mission

Find and fix the deadlock in the doorbell-DMA path of
`of_gpu_draw_spans_batch` (in `src/sdk/include/of_gpu.h`).  After a few
seconds of in-game traffic the system either silently wedges or traps
to garbage.  A previous fix attempt landed but the freeze still
reproduces, so the prior root-cause theory was wrong or incomplete.

## What you know

### Symptom

After ~5–10 seconds of normal Duke3D gameplay (E1L1, walking and
shooting) the system stops making forward progress.  Last trap dump
captured before the workaround landed:

```
mcause = 0x00000002    illegal instruction
mepc   = 0x1032be44    unmapped SDRAM
mtval  = 0x0786471f    garbage instruction word
```

Control flow jumped to garbage in unmapped SDRAM.  No
`__builtin_trap` from `of_gpu_wait`'s 50M-spin watchdog ever fired,
so the CPU was still running real code when it derailed — consistent
with **memory corruption** (something wrote into the .text region
or into a stack frame return address) rather than a pure GPU spin.

### Bisection results (definitive)

These have been verified by toggling exactly one line in the SDK at
a time, then deploying to real hardware:

| Variable | Setting | Result |
|---|---|---|
| `of_gpu_draw_spans_batch` path | per-span MMIO fallback (force `1 ||` in the `_gpu_batch_buf == NULL` guard) | **No freeze**, runs hours, ~34 fps |
| `of_gpu_draw_spans_batch` path | doorbell-DMA enabled (default) | **Freezes within seconds** |
| `OF_GPU_BATCH_MAX_SPANS` | 128 (default) — ~14 batches/frame, 7680-byte payloads | Freezes |
| `OF_GPU_BATCH_MAX_SPANS` | 32 — ~58 batches/frame, 1920-byte payloads | **Still freezes** |
| Per-span colormap_id wire format | reverted to sticky SET_COLORMAP_ID | Still freezes |

### What this rules out

- **Span encoding** — same encoder is used in both paths.  MMIO path
  works.
- **Batch size** — 4× smaller batches still freeze.
- **Per-span colormap_id** — already reverted in the working tree;
  freeze persists.
- **Pure GPU spin** — `of_gpu_wait`'s 50M-iteration watchdog never
  fires, and the trap is on the CPU side.

### What this implicates

The deadlock / corruption is in the doorbell-DMA dispatch sequence
inside `of_gpu_draw_spans_batch`:

1. Build batch in SDRAM scratch (`_gpu_batch_buf`) via cached scalar
   stores.
2. `of_cache_flush_range(_gpu_batch_buf, payload_words * 4)`.
3. `_gpu_ring_ensure((1 + payload_words) * 4)`.
4. `_gpu_ring_write(header)` — writes header word into ring BRAM
   via `GPU_RING_DATA` MMIO (auto-incrementing port-A address).
5. `GPU_DMA_SRC = _gpu_batch_buf; GPU_DMA_LEN = payload_words; GPU_DMA_KICK = 1;`
6. `_gpu_wrptr += payload_words * 4;` (mirror only — DMA's
   end-of-kick pulse is supposed to publish `ring_wrptr` atomically).
7. `while (GPU_STATUS & GPU_STATUS_DMA_BUSY) ;`

This is small enough to audit end-to-end.

## Suspects, ranked

### 1. Stale L1 → DMA pulls garbage → GPU writes pixels into code/stack

`of_cache_flush_range` is supposed to be writeback+invalidate.  If it
silently leaves dirty lines in L1 (or only handles the *aligned*
portion of the range, etc.) the GPU's m_rd_* AXI master reads stale
SDRAM, the span gets a bogus `fb_addr`, and the GPU happily writes
pixels into wherever — eventually trashing return addresses or .text.
Matches the corruption signature: **mcause=2 with unmapped mepc and
garbage mtval is exactly what you get when a return address gets
overwritten by texture pixels.**

**Test:** bypass the cache entirely.  Use the uncached SDRAM alias
for the scratch buffer (the same trick `of_gpu_palookup_upload`
already uses — it had the *exact same* class of bug and was fixed
this way).  Concretely: route `_gpu_batch_buf` writes through
`caps->sdram_uncached_base + OF_GPU_BATCH_BUF_AXI_OFFSET` and drop
the `of_cache_flush_range` call.  If freeze stops, the cache flush is
the bug.

### 2. DMA address-generator wrap at the 16 KB ring boundary

Ring BRAM is 16 KB.  After a few batches `_gpu_wrptr` is somewhere
in the middle of the ring; a max payload (1920 words = 7680 bytes)
crosses the wrap.  If the DMA target counter increments without a
proper modulo-16 KB wrap on the BRAM port-A address, payload bytes
overshoot the ring into adjacent BRAM / DRAM.

**However:** MAX_SPANS=32 (1920-byte payload) still freezes, and
each batch is small enough that wrap is much rarer.  This makes #1
more likely than #2.

### 3. DMA / CPU port-A mux race on the header word

The CPU writes the `CMD_DRAW_SPANS_BATCH` header via `GPU_RING_DATA`
(port-A MMIO).  Then `GPU_DMA_KICK` arms.  The comment block in
`of_gpu_draw_spans_batch` already calls out a port-A mux race —
`port-A mux picks DMA when both fire same cycle` — and uses the
trailing `wait DMA_BUSY` to drain.  But there's nothing protecting
the **leading edge**: the CPU's header MMIO completes, then the CPU
writes `GPU_DMA_KICK = 1` immediately after.  Is there any
guarantee the header-MMIO write has actually committed to the BRAM
cell before the DMA starts streaming bytes from offset+4?

**Test:** insert a `GPU_STATUS` read between the header MMIO and the
`GPU_DMA_KICK` to force a round-trip and serialise the AXI bus.

### 4. `_gpu_wrptr` accounting drift

The CPU's `_gpu_wrptr` mirror is advanced by `(1 + payload_words) * 4`.
But the actual GPU `ring_wrptr` is published by the DMA's
end-of-kick pulse.  If those ever drift apart by even one word,
`_gpu_ring_ensure`'s free-space calc will be wrong on the next call,
and we either deadlock (think no space) or overwrite live commands
(think free, but GPU hasn't drained).

**Test:** after `wait DMA_BUSY` returns, read `GPU_RING_WRPTR` and
`assert(GPU_RING_WRPTR == _gpu_wrptr)`.  If they ever diverge,
we've found a leak.

## Workaround currently in PocketDukeNukem-SDK

`src/sdk/include/of_gpu.h`, top of `of_gpu_draw_spans_batch`:

```c
if (1 || _gpu_batch_buf == NULL) {
    /* Force per-span MMIO path. */
    for (int i = 0; i < count; i++)
        of_gpu_draw_span(&spans[i]);
    return;
}
```

The `1 ||` guard forces the MMIO fallback.  Performance is
essentially unchanged at this workload (~34 fps) because the original
DMA path's `wait DMA_BUSY` was eating most of the theoretical
batching win.  Dropping the `1 ||` re-enables the broken DMA path
and reproduces the freeze within seconds.

## Diagnostic helpers already in tree

- `of_gpu_draw_spans_batch` has a TEMP TRACE that prints the first
  30 batches' `rdptr / sdk_wrptr / ring_wrptr / free_bytes / dma_busy`
  at kick time.  Useful for spotting wrptr drift or stuck DMA_BUSY.
- `of_gpu_wait` has a 50M-iteration watchdog that traps via
  `__builtin_trap` (mcause=2, mtval=0xc0001073 from the `unimp`).
  Distinguishes "GPU is stuck" from "CPU walked off a cliff".
- `[perf-rooms]` print on the duke3d core shows
  `spans=N flushes=N` per frame so you can see the workload shape.

## Repro

1. Build PocketDukeNukem-SDK at HEAD (`make build CORE=duke3d`).
2. Drop the `1 || ` from `of_gpu.h:572` to re-enable the DMA path.
3. Build, deploy `build/duke3d/` to Pocket SD card.
4. Boot, start E1L1, walk + shoot.  Freezes within ~5–10 s.

## What to deliver

1. A specific theory of which of the four suspects above is the
   actual root cause (or a fifth one we missed), with the
   single-line bisection signal that proves it.
2. A fix that lets us drop the `1 ||` workaround and run the DMA
   path indefinitely.
3. A note in `docs/bug-gpu-batch-dma-freeze.md` explaining what was
   actually wrong and why the prior fix attempt missed it.
