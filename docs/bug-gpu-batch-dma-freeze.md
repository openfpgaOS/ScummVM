# Closed: GPU hangs after a few seconds of CMD_DRAW_SPANS_BATCH traffic

## Status

**Fixed.**  Root cause: the doorbell-DMA scratch buffer
(`_gpu_batch_buf`) lived in *cached* SDRAM, and `of_cache_flush_range`
intermittently failed to fully drain dirty L1 lines before the
`GPU_DMA_KICK`.  The GPU's `m_rd_*` AXI master then pulled stale
SDRAM bytes for one or more span fields, `sp_fb_addr` decoded as
garbage, and the GPU's `m_wr_*` wrote pixels over CPU stack/.text in
SDRAM — surfacing several frames later as `mcause=2` with PC in
unmapped SDRAM.

**Fix:** route `_gpu_batch_buf` through the uncached SDRAM alias
(`caps->sdram_uncached_base + OF_GPU_BATCH_BUF_AXI_OFFSET`) and drop
the `of_cache_flush_range` call.  Every CPU encoder store now hits
SDRAM directly via the cache-bypass alias, so the GPU's reads see
committed data unconditionally — no flush dependency.  Same fix
`of_gpu_palookup_upload` already uses for the same class of bug.

The earlier port-A skid fix in `gpu_core.v` (commit `c2d6b47`) is
correct and stays — it covers a *different* race (CPU MMIO ring
write colliding with a DMA R-beat on port-A) which is real but
rarer.  Both fixes are needed; this doc covers the
`mcause=2 in seconds` failure mode the skid fix did *not* address.

## Symptom

Last observed trap before the workaround landed:

```
mcause = 0x00000002    (illegal instruction)
mepc   = 0x1032be44    (unmapped SDRAM)
mtval  = 0x0786471f    (garbage instruction word)
```

i.e. control flow jumped to garbage in unmapped SDRAM. No
`__builtin_trap` from `of_gpu_wait`'s 50M-spin watchdog ever fired,
so the process was still making forward progress when it derailed —
consistent with a stack/code corruption rather than a pure GPU spin.

## Workload at time of freeze

- Caller: Duke3D BUILD wall/sprite/floor renderers via
  `d3d_gpu_submit_span` → `d3d_gpu_flush_batch` → `of_gpu_draw_spans_batch`.
- Batch size: ~120 spans/batch (right at `OF_GPU_BATCH_MAX_SPANS` = 128).
- Batches per frame: ~14 (was ~30 before per-span colormap_id; now
  almost entirely buffer-full, no state-driven flushes).
- Spans per frame: ~1850.
- Each batch payload: 15 × ~120 = ~1800 words (7200 bytes), under the
  16 KB scratch buffer at `caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET`.

## Bisection signal

In `src/sdk/include/of_gpu.h`, change the line at the top of
`of_gpu_draw_spans_batch`:

```c
if (1 || _gpu_batch_buf == NULL) {     /* freeze gone */
if (   _gpu_batch_buf == NULL) {       /* freeze returns within seconds */
```

Same span encoder, same caller, same scratch buffer — the only thing
that changes is whether the helper emits N standalone `CMD_DRAW_SPAN`
commands via per-word MMIO, or builds the batch in SDRAM and uses
`CMD_DRAW_SPANS_BATCH` + `GPU_DMA_KICK`. So the bug lives in:

1. The DMA path itself (m_rd_* AXI master pulling from SDRAM into
   the ring BRAM), or
2. The header-MMIO + DMA atomic publish handoff (CPU writes the
   `CMD_DRAW_SPANS_BATCH` header word into the ring BRAM via
   `GPU_RING_DATA`, then DMA streams the payload and lifts ring_wrptr
   to expose the whole command), or
3. The `port-A mux picks DMA when both fire same cycle` path the
   helper's existing comment block flags as the reason for the
   `wait DMA_BUSY` at the end of every batch.

## Suspects worth chasing first

- **port-A mux**: helper claims the trailing `wait DMA_BUSY` covers
  CPU↔DMA contention on ring BRAM port-A. Verify in RTL that the
  next `_gpu_ring_write` after `wait DMA_BUSY` returns truly cannot
  collide with a still-in-flight DMA write — including the case where
  the CPU's MMIO write of the batch header word races the DMA's
  first beat (header is written before `GPU_DMA_KICK` but the GPU
  decoder might fetch it before DMA publishes wrptr).

- **fence-wait race**: kernel agent's prior TEMP TRACE was already
  chasing a fence-wait race in this region (per
  PocketDukeNukem-SDK conversation summary). The mcause=2 unmapped
  PC argues for memory corruption, not a stuck spin — but a
  fence-reached-but-data-stale race could let the rasteriser walk
  off into unmapped territory if a span's `fb_addr` arrived as
  stale ring bytes.

- **m_rd_* AXI bounds**: confirm the DMA respects exactly
  `payload_words` (1920 max) and never overruns into adjacent SDRAM
  (heap, stack, .text). The scratch buffer is 16 KB at offset
  `0x140000`, immediately above the 256 KB palookup window —
  if the DMA wraps wrong, it could trash palookup bytes.

## Diagnostic helpers already in tree

- `of_gpu_wait` has a 50M-iteration watchdog that traps via
  `__builtin_trap` (mcause=2 / mtval=0xc0001073 from the `unimp`).
  Distinguishes "GPU is stuck" from "CPU walked off a cliff".

## Why the prior fix attempt missed it

The first fix landed in `openfpgaOS` commit `c2d6b47` (gpu_core.v
1-deep skid for the port-A CPU/DMA collision).  That fix was *correct
but addressed a different race* — the SDK and RTL had two overlapping
bugs and we patched the harder-to-see one first:

| Bug | Trigger | Symptom | Fix |
|---|---|---|---|
| **Port-A mux race** (RTL) | CPU MMIO write to `GPU_RING_DATA` collides with a DMA R-beat in the same cycle | Dropped CPU header → stale ring_bram cell → bad `sp_fb_addr` decode | gpu_core.v 1-deep skid (`c2d6b47`) |
| **Cache flush race** (SDK) | `of_cache_flush_range` leaves a dirty L1 line in the batch payload | DMA pulls stale SDRAM → bad `sp_fb_addr` decode | `_gpu_batch_buf` via uncached alias (this fix) |

Both produce the same downstream signature: a span with corrupt
`fb_addr` causes the GPU's `m_wr_*` to write pixels into CPU code or
stack, and the trap surfaces several frames later as `mcause=2` with
unmapped `mepc`.  After landing the port-A skid, the freeze rate
dropped — fewer collisions made it through — but didn't go to zero
because the cache-flush race fires on a different timeline.  Both
fixes are needed.

The clue we under-weighted: bisection showed the freeze persisted
even with `OF_GPU_BATCH_MAX_SPANS=32` (4× smaller batches, ~4× rarer
ring-wrap, but also ~4× rarer port-A collisions).  Cache flush bugs
fire per-batch regardless of batch size, which matched the data
better than collision rate did.  In hindsight that was the signal to
audit the cache flush path first, not the RTL.

## Reproducing the original bug

1. Clone PocketDukeNukem-SDK at HEAD before this fix.
2. In `src/sdk/include/of_gpu.h`, drop the `1 ||` guard at
   `if (_gpu_batch_buf == NULL)` to re-enable the DMA path.
3. `make build CORE=duke3d`, deploy to Pocket.
4. Start a new game on the first level (E1L1).
5. Hangs within ~5–10 seconds of moving / shooting.

After the fix: drop the `1 ||` guard, redeploy, the DMA path runs
indefinitely.
