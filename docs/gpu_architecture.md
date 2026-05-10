# OpenFPGA 3D Accelerator Architecture

> Sections 2–7 describe the original design intent.  Some features
> listed there (alpha test, blend, gouraud RGB combine, indexed
> drawing, batched-span command, set-associative tex cache) were
> **never implemented or were removed during phase reduction** — see
> §1.1 below for the authoritative list of what the current GPU
> actually does, and §3.2 for the live command opcodes.

## 1. Overview

A fixed-function 3D rasterizer built around a **triangle pipeline with span fast-path**. The CPU handles all 3D transforms and submits screen-space geometry. The GPU rasterizes, textures, lights, depth-tests, and writes pixels to the framebuffer — all while the CPU runs game logic in parallel.

## 1.1. Implemented Primitives

The following is the canonical list of features that ship in the current bitstream (Pocket target, GPU at 100 MHz).  Anything not on this list is design-intent only.

### Drawing commands

| Cmd | Opcode | Payload | Implemented behaviour |
|-----|--------|---------|----------------------|
| `NOP`              | `0x01` | 0 words   | Padding/alignment. |
| `FENCE`            | `0x02` | 1 word    | Latch token into `GPU_FENCE_REACHED`. |
| `CLEAR`            | `0x10` | 2 words   | Clear FB and/or Z-buffer (`OF_GPU_CLEAR_COLOR`/`_DEPTH`).  Hardcoded 320×200 extent. |
| `CLEAR_RECT`       | `0x11` | 3 words   | Clear an arbitrary rect.  Word 0 = absolute byte addr (CPU pre-computes `fb_base + y*stride + x`); word 1 = `{w[31:16], h[15:0]}` (pixels × rows); word 2 = `{16'b0, color[15:0]}` (low byte replicated 4× per FB word).  Word-aligned full-width strips hit the 4-byte fast path; arbitrary x/w byte-strobes the partial-word edges.  Used to retire CPU `memset(frameplace, …)` paths (letterbox, status bar, menu panes, splash). |
| `SET_TEXTURE`      | `0x20` | 4 words   | Set `st_tex_addr` + `st_tex_width` for triangle path; format/wrap fields decoded but not consumed by the datapath. |
| `SET_DEPTH_FUNC`   | `0x21` | 1 word    | `NONE`/`ALWAYS`/`LESS`/`LEQUAL`/`EQUAL`/`GEQUAL`/`GREATER`/`NOTEQUAL` for triangle path. |
| `SET_FB`           | `0x23` | 2 words   | `addr`, `stride` for triangle path. |
| `SET_ZB`           | `0x24` | 2 words   | `addr`, `stride` for triangle path. |
| `SET_COLORMAP_ID`  | `0x28` | 1 word    | `[3:0] = palookup slot` (0..15).  Sticky — affects subsequent `SPAN_COLORMAP` draws.  Reset default = 0.  Each slot's 16 KB table lives in SDRAM at `0x100000 + slot*0x4000`; the GPU reads via `gpu_tex_cache` port B.  Use `of_gpu_palookup_upload(slot, …)` to populate. |
| `DRAW_TRIANGLES`   | `0x30` | `1+6N`    | Convex triangles, top-left fill rule, edge-walk → row-span emit. Per-pixel attributes: `S`/`T` (texture, multiply-mode + POT wrap), `Z` (depth), `R` (Gouraud light, 8-bit walked into the colormap row). Optional perspective via per-vertex `W`. |
| `DRAW_SPAN`        | `0x40` | 18 words  | Single span (horizontal floor or vertical column). All fields below. |

Opcodes `0x22` (`SET_BLEND`), `0x25` (`SET_SHADE_MODE`), `0x31` (`DRAW_INDEXED`), and `0x41` (`DRAW_SPANS_BATCH`) listed in legacy docs are **not implemented**.  Their SDK helpers were removed; sending one is a NOP at best.  `0x27 SET_SKIP_ZERO` is reserved for the triangle internal-span emit path; do not reuse.

### Span flags (`of_gpu_span_t.flags`)

| Bit | Name | Behaviour |
|-----|------|-----------|
| 0 | `OF_GPU_SPAN_COLORMAP`     | Texel goes through `colormap[light * 256 + texel]`.  Without this flag the texel reaches the FB unmodified. |
| 1 | `OF_GPU_SPAN_COLUMN`       | (Vestigial — fb_stride alone now controls column vs row walk.) |
| 2 | `OF_GPU_SPAN_SKIP_ZERO`    | Color-key transparency: discard texels of value `0xFF`.  This is the only "alpha-ish" path; no real blending. |
| 3 | `OF_GPU_SPAN_DEPTH_TEST`   | Compare span Z against SRAM Z-buffer using current `SET_DEPTH_FUNC`. |
| 4 | `OF_GPU_SPAN_DEPTH_WRITE`  | Write span Z back to SRAM Z-buffer (typically paired with `_TEST`). |
| 5 | `OF_GPU_SPAN_PERSP`        | Per-pixel perspective: GPU walks `s/z`, `t/z`, `1/z` in 8-pixel segments and emits affine sub-segments.  Requires the perspective payload words. |
| 6 | `OF_GPU_SPAN_TRANSLUC`     | Translucent compositing through the 32 KB on-chip `transluc[]` LUT (BUILD-style indexed-color blend).  GPU reads the destination FB byte, composes a 15-bit key `{shaded_src[7:1], fb_byte}`, looks up the blended byte in `transluc_bram`, and writes back.  Pairs with `OF_GPU_SPAN_COLORMAP` (the source byte is the post-shade palette index). |
| 7 | `OF_GPU_SPAN_TRANSLUC_REV` | Variant of TRANSLUC: swaps the LUT key axes (`{fb_byte[7:1], shaded_src}`).  Used for additive vs 50/50 blend variants. |

### Texture addressing

Multiply-mode only.  `addr = tex_base + (t_int & tex_h_mask) × tex_width + (s_int & tex_w_mask)`, where `tex_w_mask`/`tex_h_mask` come from CMD_DRAW_SPAN word 8 (`= tex_w-1` / `tex_h-1` for POT wrap; `0` decodes to `0xFFFF` = no wrap, the legacy default).  The shift-mode path (`(t>>shift)<<bits | (s>>(32-bits))`) was retired; BUILD/Quake floors use multiply-mode + POT masks.  Texture format is fixed at I8; `RGB565` exists in the enum but the datapath only handles 8-bit texels.

### Color combine

| Path | Output |
|------|--------|
| Span/triangle, `SPAN_COLORMAP` set        | `palookup[st_colormap_id][light × 256 + texel]`, served via `gpu_tex_cache` port B (SDRAM-backed; on-chip `cmap_bram` was retired) |
| Span/triangle, `SPAN_COLORMAP` clear      | `texel` (raw I8) |
| Span/triangle, `SKIP_ZERO` set, texel == 0xFF | discard pixel |
| Span/triangle, `SPAN_TRANSLUC` set        | `transluc_bram[{shaded_src[7:1], fb_byte}]` (32 KB / 128×256 quantised LUT in M10K; reads the existing FB byte via `gpu_tex_cache` AXI master, composes the key, looks up the blended byte, writes back through the FB accumulator) |
| Span/triangle, `SPAN_TRANSLUC_REV` set    | Same as TRANSLUC but with the key axes swapped: `transluc_bram[{fb_byte[7:1], shaded_src}]` |

Triangles with `R` set per vertex walk the light gradient per pixel (Gouraud "shade" — Phase 4d).  No RGB modulate, no alpha blend, no alpha test.

### Depth

16-bit depth values stored in external SRAM (256 KB available, 320×240×2 = 154 KB used).  Read-compare-write pipeline through the FBSS sub-FSM.  Functions listed above.

### State and status

| MMIO offset | Reg | Direction | Notes |
|------------|------|-----------|-------|
| `0x00` | `GPU_CTRL`            | W | bit 0 = enable, bit 1 = soft_reset, bit 2 = ring_reset |
| `0x04` | `GPU_RING_WRPTR`      | W | CPU write pointer — kicks GPU |
| `0x08` | `GPU_RING_DATA`       | W | Push one word into the ring BRAM (auto-increment) |
| `0x10` | `GPU_RING_RDPTR`      | R | GPU read pointer |
| `0x14` | `GPU_STATUS`          | R | Busy + ring_empty + state + pipeline + cache + FBSS bits |
| `0x18` | `GPU_FENCE_REACHED`   | R | Last completed fence token |
| `0x1C` | `GPU_STAT_PIXELS`     | R | Pixel counter (GPU_STATS build only) |
| `0x20` | `GPU_CMAP_ADDR`       | W | LUT upload address (auto-inc).  Bit 31 = target select: `0` = colormap (legacy, now a no-op since `cmap_bram` was retired — palookups live in SDRAM), `1` = `transluc[]`.  Low 15 bits = byte offset within the selected target. |
| `0x24` | `GPU_CMAP_DATA`       | W | LUT upload data (32-bit word, addr += 4).  Only the `transluc[]` target writes are meaningful now; colormap target writes are dropped (palookup uploads go through `of_gpu_palookup_upload` → SDRAM). |
| `0x28` | `GPU_TEX_FLUSH`       | W | Pulse to invalidate the tex cache (S_INIT walk-clears 1024 sets) |
| `0x2C` | `GPU_STAT_SPANS`      | R | Span counter (GPU_STATS only) |

### SDK entry points (header-only, in `of_gpu.h`)

Init/sync: `of_gpu_init`, `of_gpu_kick`/`_kick_verified`, `of_gpu_fence`/`_submit`/`_fence_reached`/`_wait`/`_finish`/`_shutdown`, `of_gpu_nop`, `of_gpu_ring_free`/`_can_fit`.

State / draws: `of_gpu_set_framebuffer`, `of_gpu_set_zbuffer`, `of_gpu_depth_test`, `of_gpu_bind_texture`, `of_gpu_draw_span`, `of_gpu_draw_triangles`/`_batch`, `of_gpu_clear`, `of_gpu_clear_rect`, `of_gpu_stat_pixels`, `of_gpu_stat_spans`.

LUT uploads / colormap selection: `of_gpu_colormap_upload` (slot-0 wrapper, retained for legacy callers), `of_gpu_palookup_upload(slot, data, size)` (writes to `caps->sdram_base + 0x100000 + slot*0x4000`, then `of_cache_clean_range` so the GPU's tex_cache fill sees committed bytes), `of_gpu_set_colormap_id(slot)` (sticky 4-bit slot select for subsequent SPAN_COLORMAP draws), `of_gpu_translucency_upload(table, 65536)` (decimates a BUILD-style 64 KB table to the fabric's 32 KB / 128×256 quant LUT during upload via the shared `GPU_CMAP_ADDR/DATA` MMIO with bit 31 = 1).

Removed (no datapath behind them): `of_gpu_blend`, `of_gpu_alpha_ref`, `of_gpu_shade_mode`.

```
    CPU (VexRiscv @ 100 MHz)
         |
    [SDRAM Command Ring Buffer]      ← CPU writes commands here
         |
    ┌────┴─────────────────────────────────────────────────┐
    │  GPU (~100 MHz, 5K ALMs, 64 M10K, 6 DSP)            │
    │                                                       │
    │  [DMA Fetch] → [Cmd Decode] → [State Registers]      │
    │                      |                                │
    │              ┌───────┴───────┐                        │
    │              │               │                        │
    │       [Triangle Setup]  [Span Bypass]                 │
    │       (edge equations)  (direct params)               │
    │              │               │                        │
    │              └───────┬───────┘                        │
    │                      │                                │
    │              [Scanline Rasterizer]                     │
    │              (walk edges, emit fragments)              │
    │                      │                                │
    │              [Fragment Processor]                      │
    │              ├─ gpu_tex_cache, dual-port (16 KB M10K)  │
    │              │     port A → texture fetch (p1)         │
    │              │     port B → palookup fetch (p2)        │
    │              ├─ transluc_bram (32 KB M10K, BUILD blend)│
    │              └─ Gouraud light walk                     │
    │                      │                                │
    │              [Depth Test] ←→ SRAM Z-buffer             │
    │                      │                                │
    │              [FB Writer + transluc RMW] → AXI4 → SDRAM │
    └───────────────────────────────────────────────────────┘
```

## 2. Resource Budget

| Component               | ALMs      | M10K | DSP | Notes                                    |
|--------------------------|-----------|------|-----|------------------------------------------|
| DMA command fetch        | 300       | 4    | 0   | AXI4 read, ring pointer management       |
| Command decoder + state  | 300       | 2    | 0   | Unpack commands, sticky state registers   |
| Triangle setup           | 1000      | 4    | 4   | Edge equations, attribute gradients (sequential DSP) |
| Scanline rasterizer      | 700       | 4    | 0   | Edge walking, fragment emission           |
| Span bypass path         | 150       | 0    | 0   | Mux span params into rasterizer          |
| Fragment processor       | 1000      | 0    | 2   | Texcoord step, color interp, alpha test  |
| Texture + palookup cache | 450       | 19   | 0   | 16 KB direct-mapped, 16B lines, AXI4 fill, **dual-port** (A = textures, B = palookups via SDRAM) |
| Translucency LUT         | 60        | 32   | 0   | 32 KB / 128×256 BUILD-shape blend table; CPU upload via shared `GPU_CMAP_ADDR/DATA` (bit 31 = transluc target) |
| Z-buffer interface       | 400       | 2    | 0   | SRAM read/compare/write pipeline          |
| Framebuffer writer       | 350       | 4    | 0   | 4-byte accumulator, AXI4 burst writes    |
| Misc (FIFO, fence, perf) | 300       | 4    | 0   | Fence counter, stat counters             |
| **Total**                | **4900**  | **60** | **6** |                                      |
| **Budget**               | **5000**  | **64** | **~59** |                                    |

Headroom: ~100 ALMs, 4 M10K.

> **Note (2026-04):** the table above is the original design-intent
> sketch.  Current fitted utilisation on the Pocket bitstream is
> higher after the perspective + transluc[] + dual-port-cache work
> landed:
> - ALMs: ~16,950 of 18,480 (~92%).
> - M10K: 255 of 308 (83%) — `cmap_bram` retired in favour of
>   SDRAM-backed palookups via `gpu_tex_cache` port B, freeing 16
>   M10K vs the prior 271/308.
> - DSP: 36 of 66.
>
> Per-commit cost deltas are tracked in `openfpgaOS/CHANGES.md`.

### 2.1. Expansion Options (if more ALMs available)

| Feature                   | Extra ALMs | Extra M10K | What it enables                     |
|---------------------------|-----------|------------|--------------------------------------|
| Perspective correction    | +400      | +1         | Per-16px subdivision (Quake quality) |
| Bilinear texture filter   | +300      | +2         | Smoother textures (SM64, Wipeout)    |
| Turbulence warp           | +150      | +1         | Quake water/lava/slime effects       |
| 4-column batch mode       | +300      | 0          | Duke3D vlineasm4 optimization        |
| Translucency (FB readback)| +400      | 0          | Duke3D translucent walls             |
| Edge walker coprocessor   | +700      | +2         | HW triangle-to-span decomposition    |

## 3. Command Ring Buffer

### 3.1. Memory Layout

```
SDRAM:  [ ... app data ... | Command Ring (64KB) | ... ]
                             ^                 ^
                             |                 |
                          rd_ptr (GPU)      wr_ptr (CPU)
```

- 64KB default ring buffer in SDRAM
- GPU has a DMA engine that reads commands from the ring
- CPU writes commands sequentially, updates `wr_ptr` register on `kick()`
- GPU advances `rd_ptr` as it completes commands
- Fence commands write a sequence number to a status register the CPU can poll

### 3.2. Command Packet Format

All commands are 32-bit word aligned. First word encodes type + size.

```
Word 0:  [31:24] cmd_type  [23:0] payload_words
Word 1+: command-specific payload
```

See §1.1 for the live command set.  `0x22 SET_BLEND`, `0x25 SET_SHADE_MODE`, `0x31 DRAW_INDEXED`, and `0x41 DRAW_SPANS_BATCH` are documented but **not implemented**; the others are.  `DRAW_SPAN` is **18 payload words** (not 16): the original 16 plus word 8 (`{tex_h_mask, tex_w_mask}`) plus the perspective tail.

### 3.3. Vertex Encoding in Ring (6 words = 24 bytes)

```
Word 0: [31:16] x (12.4)    [15:0] y (12.4)
Word 1: [31:16] z (16-bit)  [15:0] pad
Word 2: s (16.16 fixed-point)
Word 3: t (16.16 fixed-point)
Word 4: w (16.16 fixed-point, 1/W for perspective, 0x10000 for affine)
Word 5: [31:24] a  [23:16] b  [15:8] g  [7:0] r
```

## 4. Triangle Pipeline

### 4.1. Triangle Setup (~15-25 cycles per triangle)

Computes edge equations and attribute gradients using a shared DSP multiplier (sequential, 4 DSP blocks time-shared).

**Edge equations** (3 edges):
```
E(x,y) = A*x + B*y + C
where A = y1 - y0, B = x0 - x1, C = x1*y0 - x0*y1
```

**Attribute gradients** (Z, S, T, W, R/light):
```
dAttr/dx = ((A1*dY20) - (A2*dY10)) / (dX10*dY20 - dX20*dY10)
dAttr/dy = ((A2*dX10) - (A1*dX20)) / (dX10*dY20 - dX20*dY10)
```

The reciprocal `1/det` is computed once using a CLZ + reciprocal LUT + DSP correction multiply (same approach as PocketQuake's perspective unit, ~8-10 cycles).

Setup is amortized: at 320x200, a typical triangle covers 50-500 pixels, making 20-cycle setup negligible.

### 4.2. Scanline Rasterizer (~1 cycle per pixel)

Walks the triangle top-to-bottom, left-to-right:
1. For each scanline Y in the triangle's bounding box:
   - Compute left/right X bounds from edge equations
   - For each pixel X in [left, right]:
     - Emit fragment with interpolated Z, S, T, W, color

The rasterizer uses integer edge stepping (add `A` per X step, add `B` per Y step) — no multiplies in the inner loop.

**Sub-pixel precision**: 12.4 coordinates give 1/16 pixel accuracy. Fill rules (top-left) prevent pixel gaps between adjacent triangles.

### 4.3. Span Bypass

When a DRAW_SPAN command arrives, it skips triangle setup entirely. The span's pre-computed parameters (start coord, step, count) feed directly into the fragment processor. The rasterizer just counts pixels using the span's `count` field.

Two texture addressing modes (selected by span flags):
- **Multiply mode** (Quake): `addr = base + (t >> 16) * width + (s >> 16)`
- **Shift mode** (Duke3D/Doom): `addr = base + ((t >> shift) << bits) | (s >> (32-bits))`

### 4.4 Fragment Processor (~2-4 cycles per pixel)

Per fragment:
1. **Texture address**: compute from S, T (or S/W, T/W with perspective divide)
2. **Texture fetch**: read texel from texture cache
3. **Color combine**:
   - I8 + colormap: `output = colormap[light * 256 + texel]`
   - I8 no colormap: `output = texel`
   - RGB565 + gouraud: `output = texel * vertex_color` (DSP multiply)
4. **Alpha test**: discard if alpha < ref
5. **Depth test**: compare fragment Z against SRAM Z-buffer
6. **Framebuffer write**: accumulate bytes, burst-write to SDRAM

### 4.5. Texture Cache (as built)

- **Capacity**: 16 KB.  `data_mem` = 1024 sets × 4 words × 32 bits, all in M10K.
- **Organization**: **direct-mapped**, 1024 sets, 16-byte lines (16 I8 texels per line).  Tag is 12 bits.  `addr[3:0]` = byte offset, `addr[13:4]` = set, `addr[25:14]` = tag.
- **Hit latency**: 1 cycle (registered M10K read; consumer's combinational `byte_from_rd` selects the correct lane).
- **Miss latency**: AXI4 burst fill of 4 words (~10–20 cycles depending on SDRAM).  Cache stalls while filling.
- **Flush**: `GPU_TEX_FLUSH` MMIO pulse → walk-clear all 1024 valid bits via `S_INIT`.
- **Read latch is gated on accept**: `rd_*` only update when `req_valid && req_ready` so pipeline stalls (FBSS mid-write) don't let `req_addr` drift away from `pipe_addr` and corrupt `byte_from_rd`.  See `tb_gpu_floor_span` and `tb_gpu_gpudemo` for the regression coverage.

### 4.6. Colormap BRAM

- **Capacity**: 16 M10K = 16KB
- **Layout**: 64 rows × 256 columns = 16384 entries
- **Access**: `colormap[light_level * 256 + texel_index]` → 8-bit output color
- **Upload**: DMA from SDRAM at init time via `of_gpu_colormap_upload()`
- **Shared by**: Doom (colormap.lmp), Duke3D (palookup), Quake (colormap), Descent (lighting tables)

### 4.7. Depth Buffer (SRAM)

- **Format**: 16-bit unsigned per pixel
- **Storage**: External SRAM (256KB available, 320×200×2 = 128KB needed)
- **Interface**: dedicated SRAM port, read-compare-write pipeline
- **Latency**: 1-2 cycles (SRAM is fast)
- **Functions**: less, less-equal, equal, always, none

## 5. Per-Game Mapping

### 5.1. Doom

Doom renders vertical columns (walls) and horizontal spans (floors/ceilings). Pure span engine.

```c
// Wall column
of_gpu_span_t col = {
    .fb_addr  = dest_addr,
    .tex_addr = texture_addr,
    .s        = frac,           // texture V position (16.16)
    .t        = 0,              // unused (1D column)
    .sstep    = fracstep,       // V step per pixel
    .tstep    = 0,
    .count    = column_height,
    .light    = light_level,
    .flags    = OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_COLUMN,
    .fb_stride = 320,           // advance one row per pixel
    .tex_width = tex_height,    // 1D column texture
};
of_gpu_draw_span(&col);
```

### 5.2. Duke3D (Build Engine)

Same span model as Doom.  BUILD's `hlineasm4` shift-mode addressing is reproduced via POT wrap masks on multiply-mode (the shift-mode datapath itself was retired):

```c
// Floor/ceiling span (hlineasm4 — multiply-mode + POT masks)
of_gpu_span_t span = {
    .fb_addr    = dest_addr,
    .tex_addr   = texture_addr,
    .s          = i4 >> (16 - width_bits),       // 0.32 i4 → 16.16 sp_s
    .t          = i5 >> (shifter - 16),          // 0.32 i5 → 16.16 sp_t
    .sstep      = -(int32_t)(asm2 >> (16 - width_bits)),
    .tstep      = -(int32_t)(asm1 >> (shifter - 16)),
    .count      = pixel_count,
    .light      = shade,
    .flags      = OF_GPU_SPAN_COLORMAP,
    .fb_stride  = -1,                            // walk left, matching SW dest--
    .tex_width  = 1u << width_bits,
    .tex_w_mask = (1u << width_bits) - 1,         // POT wrap, mod tex_w
    .tex_h_mask = (1u << (32 - shifter)) - 1,     // POT wrap, mod tex_h
};
of_gpu_draw_span(&span);
```

Both `tex_w` and `tex_h` must be powers of two (always true for BUILD textures).  See `src/duke3d/d3d_gpu.c` in PocketDukeNukem-SDK for the full helper.

### 5.3. Quake

Two options:

**Option A: Span path (like PocketQuake)**
Use `of_gpu_draw_span()` with `OF_GPU_SPAN_PERSP` for perspective-corrected world surfaces, and plain spans for alias models. Closest to existing PocketQuake code — easiest port.

**Option B: Triangle path (like GLQuake)**
Submit BSP surfaces as triangle fans. The GPU handles rasterization and texturing.
```c
of_gpu_bind_texture(&surface_tex);
of_gpu_depth_test(OF_GPU_DEPTH_LESS);
of_gpu_draw_triangles(surface_verts, surface_tri_count * 3);
```

Option B is more general but requires more CPU work for surface cache / lightmap application. Option A is faster for Quake specifically.

### 5.4. Quake 2 / Half-Life

Triangle-based. Quake 2 has a software renderer, but the OpenGL path maps naturally:

```c
// Per surface: base texture × lightmap
of_gpu_bind_texture(&base_texture);
of_gpu_depth_test(OF_GPU_DEPTH_LESS);
// Vertex R = pre-computed light level from lightmap
of_gpu_draw_triangles(surface_verts, tri_count * 3);
```

Multitexture (base × lightmap in one pass) would require a second texture unit — not in the initial 5K ALM budget. Workaround: pre-multiply lightmap into a surface cache (like Quake's software renderer), or do lightmap as a second pass with multiply blend.

### 5.5. Super Mario 64

Triangle-based with vertex colors.  Today's pipeline supports per-vertex `R` (8-bit Gouraud light walked through the colormap row) — `g`/`b` channels are decoded into the vertex struct but not consumed by the datapath.  Full RGB modulate would need a second-texture combiner unit (not in the current build).

```c
of_gpu_bind_texture(&mario_texture);
of_gpu_depth_test(OF_GPU_DEPTH_LESS);

of_gpu_vertex_t v[3] = {
    { x0, y0, z0, 0, s0, t0, OF_GPU_FP16(1), r0, 0, 0, 255 },
    { x1, y1, z1, 0, s1, t1, OF_GPU_FP16(1), r1, 0, 0, 255 },
    { x2, y2, z2, 0, s2, t2, OF_GPU_FP16(1), r2, 0, 0, 255 },
};
of_gpu_draw_triangles(v, 3);
```

### 5.6. Descent

Textured polygons with flat/Gouraud shading, 8-bit indexed. Triangle pipeline + colormap.

### 5.7. Wipeout

Textured triangles with Gouraud shading.  Single-channel light works today; semi-transparency does **not** (no blend unit) — translucent surfaces have to stay in software or be approximated with `OF_GPU_SPAN_SKIP_ZERO` color-key.

## 6. Performance Estimates

At 100 MHz, 320×200 (64,000 pixels):

| Configuration               | Cycles/Pixel | Pixels/sec | FPS (fill-limited) |
|-----------------------------|-------------|------------|---------------------|
| Triangle + affine + I8+cmap | ~4-5        | 20-25M     | 312-390             |
| Triangle + affine + RGB565  | ~4-5        | 20-25M     | 312-390             |
| Triangle + perspective + I8 | ~8-12       | 8-12M      | 130-190             |
| Span (Doom/Duke3D style)    | ~3-5        | 20-33M     | 312-520             |
| Span + perspective (Quake)  | ~8-12       | 8-12M      | 130-190             |
| Clear (DMA)                 | ~0.25       | 400M       | N/A                 |

**Fill rate is never the bottleneck at 320×200.** Even the slowest mode (perspective) can fill the screen 2-3× per frame at 30 FPS. The real bottleneck is CPU-side transform, BSP traversal, and game logic — which is exactly why the async model matters.

### 6.1. Overdraw

Typical overdraw ratios:
- Doom: 1.5-2× (no z-buffer, painter's order)
- Duke3D: 1.5-2× (same)
- Quake: 1.2-1.5× (z-buffered, front-to-back BSP)
- SM64: 2-3× (many overlapping polygons)
- Wipeout: 1.5-2×

At 2× overdraw, 128K effective pixels per frame. Still well within fill budget.

## 7. Implementation Status

### Shipped

- Phase 1 — span pipeline: ring + DMA + decode + texture cache (16 KB direct-mapped, M10K) + colormap BRAM (16 KB, 64×256) + accumulating FB writer + SRAM Z interface + fence/MMIO surface.
- Phase 2 — triangle pipeline: convex-only edge walk (top-left fill rule, 12.4 sub-pixel), per-pixel `S`/`T`/`Z`/`R` interpolation, depth test+write, top-of-pipeline shared with span path via `S_FRAG_PIPE`.
- Phase 3 partial — perspective: 8-pixel affine sub-segments using projection-space `s/z`/`t/z`/`1/z` plus a CLZ + reciprocal LUT + Newton-Raphson refine (works on both `DRAW_SPAN` with `SPAN_PERSP` and triangle path with per-vertex `W`).
- Phase 4 — write coalescing: 4-byte FB accumulator, AXI burst writes; mid-flight cache flush handling.

### Not implemented (despite enums / placeholders that may exist in headers)

- Real alpha blending (`SET_BLEND`, `OF_GPU_BLEND_ALPHA/_ADD`).  Only color-key transparency via `SPAN_SKIP_ZERO`.
- Alpha test (`SET_ALPHA_REF`).
- Full RGB Gouraud modulate.  Only the 8-bit `R` light walk lands in the colormap row.
- Bilinear filtering.
- Indexed primitives (`DRAW_INDEXED`).
- Batched span command (`DRAW_SPANS_BATCH`).
- Multi-texture / lightmap combine.
- RGB565 texel sampling (enum exists; datapath is I8-only).
- Texture wrap modes other than POT-mask + the implicit wrap on the multiply addressing.
- Triangles outside convex / counter-clockwise orientation.

### Verification harnesses (`src/fpga/test/`)

- `tb_gpu`            — 249 focused tests (spans, triangles, depth, colormap, perspective, cache flush mid-flight).
- `tb_gpu_floor_span` — 8 BUILD `hlineasm4` cases against a CPU oracle, validating multiply-mode + POT wrap.
- `tb_gpu_gpudemo`    — long-running per-frame submission stress, mirrors gpudemo's pattern.

### Open future work

- Alpha blending (blend unit + a second FB read port).
- Bilinear filter (one extra texel-fetch port + 4-tap combiner).
- 4-column batched mode (Duke3D `vlineasm4` analogue).
- Edge walker coprocessor (HW triangle-to-span decomposition; would let CPU drive at higher tri rates).
- Proper triangle backface / CCW handling.
