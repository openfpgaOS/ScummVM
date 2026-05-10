/*
 * of_gpu.h -- Hardware GPU Accelerator API for openfpgaOS
 *
 * Asynchronous span + triangle rasteriser.  CPU submits commands to a
 * 16 KB ring buffer in GPU-internal M10K BRAM; the GPU processes them
 * in parallel, writing pixels to the framebuffer via AXI4.
 *
 * Ring buffer: 16 KB in M10K (no cache coherence issues).
 * CPU writes ring data via MMIO (GPU_RING_DATA, auto-increment).
 * CPU kicks GPU by writing GPU_RING_WRPTR.
 *
 * IMPORTANT: This header contains static mutable state (_gpu_wrptr, etc).
 * Include it from exactly ONE translation unit per program.
 */

#ifndef OF_GPU_H
#define OF_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>
#include <stdlib.h>     /* malloc — used by of_gpu_init for the batch scratch buffer */

#ifndef OF_PC
#include "of_caps.h"
#include "of_cache.h"   /* of_cache_clean_range() — used by palookup upload + batch */
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define OF_GPU_CLEAR_COLOR      (1 << 0)
/* bit 1 reserved (was OF_GPU_CLEAR_DEPTH — Z buffer dropped) */

#define OF_GPU_RING_SIZE        16384   /* 16 KB M10K BRAM ring */

/* Fixed-point helpers */
#define OF_GPU_FIXED_16_16(x)   ((int32_t)((x) * 65536))   /* float → 16.16 */
#define OF_GPU_SUBPIXEL(x)      ((int16_t)((x) * 16))       /* pixel → 12.4  */
#define OF_GPU_FP16(x)          OF_GPU_FIXED_16_16(x)
#define OF_GPU_SC(x)            OF_GPU_SUBPIXEL(x)

/* ================================================================
 * Span Flags
 * ================================================================ */

#define OF_GPU_SPAN_COLORMAP     (1 << 0)
/* bit 1 reserved (was OF_GPU_SPAN_COLUMN — never wired in the RTL) */
#define OF_GPU_SPAN_SKIP_ZERO    (1 << 2)
/* bits 3/4 reserved (were SPAN_DEPTH_TEST/WRITE — Z buffer dropped) */
#define OF_GPU_SPAN_PERSP        (1 << 5)
#define OF_GPU_SPAN_TRANSLUC     (1 << 6)
/* bit 7 reserved (was OF_GPU_SPAN_TRANSLUC_REV — REV variant dropped) */

/* ================================================================
 * Data Structures
 * ================================================================ */

typedef struct {
    uint32_t fb_addr;
    uint32_t tex_addr;
    int32_t  s, t;
    int32_t  sstep, tstep;
    uint16_t count;
    uint8_t  light;
    uint8_t  flags;
    int16_t  fb_stride;
    uint16_t tex_width;
    /* POT wrap masks (tex_w - 1 / tex_h - 1).  0 means "no wrap" — the
     * default for shift-free callers.  Set both to (tex_w-1) and
     * (tex_h-1) to reproduce BUILD/Quake-style shift-mode wrap inside
     * the GPU's multiply-mode addressing.  Both dimensions must be POT. */
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
    /* Perspective (optional, requires PERSP flag) */
    int32_t  sdivz, tdivz;
    int32_t  zi_persp;
    int32_t  sdivz_step, tdivz_step;
    int32_t  zi_step;
} of_gpu_span_t;

typedef struct {
    uint32_t addr;
    uint16_t width;
    uint16_t height;
} of_gpu_texture_t;

typedef struct {
    int16_t  x, y;          /* Screen position, 12.4 fixed-point */
    uint16_t z;             /* Depth: 0 = near, 0xFFFF = far */
    uint16_t pad;
    int32_t  s, t;          /* Texture coordinates, 16.16 fixed-point */
    int32_t  w;             /* 1/W for perspective (0x10000 = affine) */
    uint8_t  r, g, b, a;   /* Vertex color / light / alpha */
} of_gpu_vertex_t;          /* 24 bytes = 6 words */

/* ================================================================
 * MMIO Registers
 * ================================================================ */

#ifndef OF_PC

/* GPU MMIO base. The kernel reports the per-target address via the
 * of_capabilities descriptor; of_gpu_init() reads it once and caches
 * it in _gpu_base, so the GPU register macros below dereference a
 * file-static variable instead of a hardcoded immediate. This is what
 * lets the same SDK app .elf run on a target whose GPU window sits
 * at a different CPU address. */
static uint32_t _gpu_base;

#define OF_GPU_REG(off)         (*(volatile uint32_t *)(_gpu_base + (off)))

#define GPU_CTRL                OF_GPU_REG(0x00)  /* W: bit0=enable, bit1=soft_reset, bit2=ring_reset */
#define GPU_RING_WRPTR          OF_GPU_REG(0x04)  /* W: CPU write pointer (byte offset) — kicks GPU */
#define GPU_RING_DATA           OF_GPU_REG(0x08)  /* W: write next word to ring BRAM (auto-increment) */
#define GPU_DMA_SRC             OF_GPU_REG(0x0C)  /* W: SDRAM byte address of command buffer to pull */
#define GPU_RING_RDPTR          OF_GPU_REG(0x10)  /* R: GPU read pointer */
#define GPU_STATUS              OF_GPU_REG(0x14)  /* R: {29'b0, dma_busy, ring_empty, busy} */
#define GPU_FENCE_REACHED       OF_GPU_REG(0x18)  /* R: last completed fence token */
#define GPU_DMA_LEN             OF_GPU_REG(0x1C)  /* W: word count to pull (≤4096) */
#define GPU_TRANSLUC_ADDR       OF_GPU_REG(0x20)  /* W: byte addr into transluc[] (auto-inc by 4) */
#define GPU_TRANSLUC_DATA       OF_GPU_REG(0x24)  /* W: 32-bit word into transluc[] */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */
#define GPU_DMA_KICK            OF_GPU_REG(0x2C)  /* W: write 1 to fire DMA pull from (SRC, LEN) */

/* GPU_STATUS bit definitions */
#define GPU_STATUS_BUSY        0x1u
#define GPU_STATUS_RING_EMPTY  0x2u
#define GPU_STATUS_DMA_BUSY    0x4u

/* ================================================================
 * Command IDs
 * ================================================================ */

#define GPU_CMD_NOP             0x01
#define GPU_CMD_FENCE           0x02
#define GPU_CMD_CLEAR           0x10
#define GPU_CMD_CLEAR_RECT      0x11  /* 3-word payload: start byte addr,
                                       * {w,h}, {pad,color}. Color's low
                                       * byte is replicated 4× per FB
                                       * word, matching CMD_CLEAR. */
#define GPU_CMD_SET_TEXTURE     0x20
/* 0x21 GPU_CMD_SET_DEPTH_FUNC retired in lean Phase 2.3 (Z dropped). */
#define GPU_CMD_SET_FB          0x23
/* 0x24 GPU_CMD_SET_ZB         retired in lean Phase 2.3 (Z dropped). */
#define GPU_CMD_SET_COLORMAP_ID 0x28  /* 1-word payload: [3:0] = palookup slot */
#define GPU_CMD_DRAW_TRIANGLES  0x30
#define GPU_CMD_DRAW_SPAN       0x40
/* CMD_DRAW_SPANS_BATCH: header carries `15*N` payload words (no count
 * word — N derives from header).  Decoder loops the existing CMD_DRAW_SPAN
 * fragment-pipe path once per 15-word span.  Saves the per-span MMIO
 * header + (when paired with the doorbell-DMA path) lets the CPU stream
 * the whole batch as cached scalar stores into SDRAM, then kick the GPU
 * to pull it via its own AXI master — eliminates the 120 ns/word MMIO
 * stall that dominates per-span dispatch cost. */
#define GPU_CMD_DRAW_SPANS_BATCH 0x41

/* Maximum spans per CMD_DRAW_SPANS_BATCH dispatch.  At 15 words/span
 * that's 1920 words = 7680 bytes per batch — fits comfortably in the
 * 16 KB ring with room for state-change commands.  of_gpu_draw_spans_batch
 * splits longer arrays across multiple kicks.  Exposed so callers
 * (e.g. game-side accumulators) can size their own buffers to flush at
 * the same boundary the SDK helper uses internally. */
#define OF_GPU_BATCH_MAX_SPANS  128

/* ================================================================
 * Palookup (colormap) layout in SDRAM — must match gpu_core.v's
 * PALOOKUP_BASE / PALOOKUP_STRIDE constants.
 *
 * Each slot holds a Quake/BUILD-shape shade × texel table.  Slot 0
 * is the default (used by callers that don't issue CMD_SET_COLORMAP_ID,
 * preserving single-palookup compatibility).  Up to 16 slots; the
 * GPU reads palookup[slot][shade][texel] from
 *   GPU_AXI_BASE + 0x100000 + slot*0x4000 + shade*256 + texel
 * via gpu_tex_cache port B (the prior on-chip cmap_bram is retired).
 *
 * The CPU-visible address depends on how the target maps the GPU's
 * AXI M0 into the CPU address space — apps should obtain it via the
 * runtime caps descriptor and add the per-slot offset.  These
 * constants encode the GPU-side AXI offset (0x100000) and per-slot
 * stride; the kernel's caps descriptor adds the per-target physical
 * base.  The lookup is target-portable as long as the kernel
 * advertises a `palookup_base` field that maps the same 26-bit
 * GPU AXI offset.
 * ================================================================ */
#define OF_GPU_PALOOKUP_AXI_OFFSET 0x00100000u  /* GPU AXI M0 byte addr of slot 0 */
#define OF_GPU_PALOOKUP_STRIDE     0x00004000u  /* 16 KB per slot */
#define OF_GPU_PALOOKUP_SLOTS      16

/* Doorbell-DMA scratch region — must live in SDRAM because gpu_core's
 * m_rd_* AXI master only reaches the SDRAM arbiter (see core_top.v's
 * sdram_arb instantiation: GPU is m0, no other targets are wired).
 * Placed right above the palookup window (0x100000..0x140000); 16 KB
 * reserved leaves headroom over the 7680-byte max batch payload. */
#define OF_GPU_BATCH_BUF_AXI_OFFSET  0x00140000u
#define OF_GPU_BATCH_BUF_BYTES       0x00004000u  /* 16 KB reserved */

/* ================================================================
 * Ring Buffer State (app-side)
 *
 * Static mutable — include this header from one .c file only.
 * ================================================================ */

static uint32_t _gpu_wrptr;
static uint32_t _gpu_fence_next;

static const uint32_t _gpu_ring_mask = OF_GPU_RING_SIZE - 1;

/* Doorbell-DMA scratch buffer.  Pinned to a fixed SDRAM offset by
 * of_gpu_init — gpu_core's m_rd_* AXI master only reaches the SDRAM
 * arbiter, so the scratch must live in SDRAM.  Apps' malloc heap
 * starts in CRAM0 territory and would be unreachable to the GPU.
 * NULL on targets that don't expose SDRAM via caps; the helper then
 * falls back to per-span MMIO. */
static uint32_t *_gpu_batch_buf;

/* ---- Internal helpers ---- */

static inline void _gpu_ring_ensure(uint32_t bytes) {
    /* Fast path: enough free space already, return immediately. */
    if (((GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask) >= bytes)
        return;

    /* Slow path: ring is full. Publish our current write pointer to
     * the GPU first — otherwise the GPU is sitting at its old wrptr
     * with nothing to drain, and we'd spin forever. (The GPU only
     * starts consuming commands when wrptr advances; queuing many
     * spans without an intervening kick deadlocks the producer when
     * the ring fills.) Then spin until enough space frees up. */
    GPU_RING_WRPTR = _gpu_wrptr;
    while (((GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask) < bytes)
        ;
}

/* Write a word to the ring BRAM via MMIO (no cache issues). */
static inline void _gpu_ring_write(uint32_t w) {
    GPU_RING_DATA = w;
    _gpu_wrptr = (_gpu_wrptr + 4) & _gpu_ring_mask;
}

static inline void _gpu_cmd_header(uint8_t cmd, uint32_t payload_words) {
    _gpu_ring_ensure((1 + payload_words) * 4);
    _gpu_ring_write(((uint32_t)cmd << 24) | (payload_words & 0x00FFFFFF));
}

/* ================================================================
 * API Functions
 * ================================================================ */

static inline void of_gpu_init(void) {
    /* Resolve the GPU MMIO base from the runtime caps descriptor.
     * Must be called after main() (or after the SDK constructors run)
     * so _of_caps_ptr is populated. Apps that try to drive the GPU
     * before of_gpu_init() will dereference a NULL _gpu_base and
     * fault clearly. */
    _gpu_base = of_get_caps()->gpu_base;

    _gpu_wrptr = 0;
    _gpu_fence_next = 1;
    GPU_CTRL = 4;               /* ring_reset: clear wr_addr + wrptr + rdptr */
    GPU_RING_WRPTR = 0;
    GPU_CTRL = 1;               /* enable */

    /* Pin the doorbell-DMA scratch buffer at a known SDRAM offset.
     * gpu_core's m_rd_* AXI master goes only to the SDRAM arbiter
     * (core_top.v: sdram_arb m0) so the scratch MUST live in SDRAM —
     * a malloc()'d pointer that early-init resolved to CRAM0 (where
     * apps' .text/.data live) is unreachable and the DMA would pull
     * garbage.  Same scheme palookup uses (caps->sdram_base +
     * OF_GPU_PALOOKUP_AXI_OFFSET); we sit one window above. */
    {
        const struct of_capabilities *caps = of_get_caps();
        if (caps && caps->sdram_base != 0)
            _gpu_batch_buf = (uint32_t *)(uintptr_t)
                (caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET);
        else
            _gpu_batch_buf = NULL;   /* helper falls back to per-MMIO */
    }
}

/* Upload a palookup table to slot N in SDRAM.  The GPU reads palookup
 * bytes through gpu_tex_cache port B; this helper writes the table
 * directly to the SDRAM region the cache pulls from, then flushes the
 * CPU L1 lines so the GPU sees committed data.  16 KB per slot, up to
 * 16 slots (cf. OF_GPU_PALOOKUP_*).
 *
 * Slot selection at draw time is sticky: call of_gpu_set_colormap_id()
 * to switch.  Reset default is slot 0, so single-palookup apps just
 * call of_gpu_palookup_upload(0, …) and never issue a SET. */
static inline void of_gpu_palookup_upload(uint8_t slot, const uint8_t *data,
                                           uint32_t size) {
    if (slot >= OF_GPU_PALOOKUP_SLOTS || size > OF_GPU_PALOOKUP_STRIDE) return;
    const struct of_capabilities *caps = of_get_caps();
    if (caps->sdram_base == 0) return;  /* target without exposed SDRAM */
    /* Write through the uncached SDRAM alias so the bytes hit DRAM
     * directly — no D-cache pollution, no flush dependency.  The GPU's
     * tex_cache port B reads the same physical bytes via AXI; on the
     * first fill it sees the committed data unconditionally.
     *
     * The prior version wrote to the cached alias + of_cache_clean_range
     * but appears to leave stale data on hardware (Quake symptom: world
     * + alias surfaces all map into a small subset of palette entries
     * — classic "palookup is mostly zeros" signature).  Uncached path
     * is one-time level-load cost so a single 16 KB byte-loop without
     * cache benefit is fine. */
    uint32_t cached_base = caps->sdram_base
                         + OF_GPU_PALOOKUP_AXI_OFFSET
                         + (uint32_t)slot * OF_GPU_PALOOKUP_STRIDE;
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)
        ((cached_base - caps->sdram_base) + caps->sdram_uncached_base);
    for (uint32_t i = 0; i < size; i++) dst[i] = data[i];
}

/* Select the active palookup slot for subsequent SPAN_COLORMAP draws.
 * Sticky state — stays in effect until the next of_gpu_set_colormap_id().
 * Default at GPU reset is slot 0 (matching the legacy single-palookup
 * behaviour). */
static inline void of_gpu_set_colormap_id(uint8_t slot) {
    _gpu_cmd_header(GPU_CMD_SET_COLORMAP_ID, 1);
    _gpu_ring_write((uint32_t)(slot & 0xF));
}

/* Decimates BUILD's 64 KB transluc[256][256] to the fabric's 32 KB
 * / 128×256 quantised LUT (low bit of source axis dropped) during the
 * upload. */
static inline void of_gpu_translucency_upload(const uint8_t *table, uint32_t size) {
    if (size != 65536) return;
    GPU_TRANSLUC_ADDR = 0;
    for (int s7 = 0; s7 < 128; s7++) {
        const uint8_t *row = &table[(s7 << 1) << 8];
        const uint32_t *row32 = (const uint32_t *)row;
        for (int w = 0; w < 64; w++)
            GPU_TRANSLUC_DATA = row32[w];
    }
}

static inline void of_gpu_kick(void) {
    GPU_RING_WRPTR = _gpu_wrptr;
}

static inline uint32_t of_gpu_fence(void) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FENCE, 1);
    _gpu_ring_write(token);
    return token;
}

static inline uint32_t of_gpu_submit(void) {
    uint32_t token = of_gpu_fence();
    of_gpu_kick();
    return token;
}

static inline int of_gpu_fence_reached(uint32_t token) {
    return (int32_t)(GPU_FENCE_REACHED - token) >= 0;
}

static inline void of_gpu_wait(uint32_t token) {
    /* Bounded spin — if the GPU hangs (fb_acc never flushes, tex cache
     * stuck in fill, pipeline deadlocked), the old unbounded spin
     * silently froze the machine with no diagnostic.  Timeout triggers
     * an illegal-instruction trap so fatal_trap dumps the GPU state;
     * the registers to inspect on the trap side are:
     *   GPU_STATUS    (0x14) — busy + ring_empty
     *   GPU_RING_RDPTR (0x10) — where the GPU last stopped fetching
     *
     * Uses a plain iteration counter rather than a cycle CSR: this
     * VexiiRiscv build is compiled without --performance-counters so
     * rdcycle / mcycle both trap illegal-instruction (mtval=0xc8002873
     * / 0xb8002873 observed).  Iteration count of 50M with a ~10-cycle
     * body gives roughly ~5 s of wait at 100 MHz — generous enough
     * that normal fence completions always beat it, tight enough that
     * a genuine hang surfaces quickly. */
    uint32_t spins = 50000000u;
    while (!of_gpu_fence_reached(token)) {
        if (--spins == 0) {
            __builtin_trap();  /* → illegal-instruction trap, mcause=2 */
        }
    }
}

static inline void of_gpu_finish(void) {
    of_gpu_wait(of_gpu_submit());
}

static inline void of_gpu_shutdown(void) {
    of_gpu_finish();
    GPU_CTRL = 0;
}

static inline void of_gpu_nop(void) {
    _gpu_cmd_header(GPU_CMD_NOP, 0);
}

static inline uint32_t of_gpu_ring_free(void) {
    return (GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline int of_gpu_ring_can_fit(uint32_t bytes) {
    return of_gpu_ring_free() >= bytes;
}

/* ---- State commands ---- */

static inline void of_gpu_set_framebuffer(uint32_t addr, uint16_t stride) {
    _gpu_cmd_header(GPU_CMD_SET_FB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
}

/* Z-buffer / depth-test API retired with the lean Z-removal in Phase 2.3.
 * Quake / SDL2 / Doom-style renderers do their own visibility (BSP /
 * paint-order); the GPU is now strictly a paint-order rasterizer. */

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 2);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(((uint32_t)tex->width << 16) | tex->height);
}

/* ---- Draw commands ---- */

/* Whole-FB clear.  flags bit 0 = clear color (the only flag still
 * accepted; the bit-1 depth-clear path was retired with the Z buffer). */
static inline void of_gpu_clear(uint32_t flags, uint16_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR, 2);
    _gpu_ring_write((flags << 16) | color);
    _gpu_ring_write(0);  /* word 1 reserved (was depth value) */
}

/* Clear a rectangular region of the framebuffer to a constant color.
 * Caller computes the start byte address (fb_base + y*stride + x); the
 * GPU walks `h` rows × `w` bytes from there, advancing each row by the
 * active st_fb_stride.  Color's low byte is replicated 4× per FB word
 * (matches CMD_CLEAR's shape).  Word-aligned full-width strips
 * (letterbox / status bar) hit the 4-byte fast path; arbitrary x/w
 * paths byte-strobe the partial-word edges.  Used to retire the last
 * per-frame CPU memset(frameplace, …) categories — see
 * project_gpu_owns_framebuffer.md. */
static inline void of_gpu_clear_rect(uint32_t start_byte_addr,
                                      uint16_t w, uint16_t h,
                                      uint8_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(start_byte_addr);
    _gpu_ring_write(((uint32_t)w << 16) | (uint32_t)h);
    _gpu_ring_write((uint32_t)color);
}

/*
 * Draw a single span.  15 payload words: 9 core + 6 perspective.
 * GPU ignores the perspective words unless OF_GPU_SPAN_PERSP is set.
 */
static inline void of_gpu_draw_span(const of_gpu_span_t *span) {
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN, 15);
    _gpu_ring_write(span->fb_addr);
    _gpu_ring_write(span->tex_addr);
    _gpu_ring_write((uint32_t)span->s);
    _gpu_ring_write((uint32_t)span->t);
    _gpu_ring_write((uint32_t)span->sstep);
    _gpu_ring_write((uint32_t)span->tstep);
    _gpu_ring_write(((uint32_t)span->count << 16) |
                    ((uint32_t)span->light << 8) |
                    ((uint32_t)span->flags));
    _gpu_ring_write(((uint32_t)(uint16_t)span->fb_stride << 16) |
                    (uint32_t)span->tex_width);
    /* Word 8: POT wrap masks (high 16 = T, low 16 = S).  Both = 0
     * means no wrap.  RTL decodes 0 as 0xFFFF internally. */
    _gpu_ring_write(((uint32_t)span->tex_h_mask << 16) |
                    (uint32_t)span->tex_w_mask);
    _gpu_ring_write((uint32_t)span->sdivz);
    _gpu_ring_write((uint32_t)span->tdivz);
    _gpu_ring_write((uint32_t)span->zi_persp);
    _gpu_ring_write((uint32_t)span->sdivz_step);
    _gpu_ring_write((uint32_t)span->tdivz_step);
    _gpu_ring_write((uint32_t)span->zi_step);
}

/* Encode one span into 15 words (the same field layout as of_gpu_draw_span's
 * payload, sans header).  Used by the batch helper to build the SDRAM
 * scratch buffer at scalar-store speed. */
static inline void _gpu_encode_span(uint32_t *p, const of_gpu_span_t *s) {
    p[0]  = s->fb_addr;
    p[1]  = s->tex_addr;
    p[2]  = (uint32_t)s->s;
    p[3]  = (uint32_t)s->t;
    p[4]  = (uint32_t)s->sstep;
    p[5]  = (uint32_t)s->tstep;
    p[6]  = ((uint32_t)s->count << 16) | ((uint32_t)s->light << 8) |
            (uint32_t)s->flags;
    p[7]  = ((uint32_t)(uint16_t)s->fb_stride << 16) | (uint32_t)s->tex_width;
    p[8]  = ((uint32_t)s->tex_h_mask << 16) | (uint32_t)s->tex_w_mask;
    p[9]  = (uint32_t)s->sdivz;
    p[10] = (uint32_t)s->tdivz;
    p[11] = (uint32_t)s->zi_persp;
    p[12] = (uint32_t)s->sdivz_step;
    p[13] = (uint32_t)s->tdivz_step;
    p[14] = (uint32_t)s->zi_step;
}

/* of_gpu_draw_spans_batch — submit N spans in one go.
 *
 * Same per-span semantics as of_gpu_draw_span; the spans are processed
 * back-to-back through the existing fragment pipeline.  No state is
 * shared between spans — callers must emit any SET_FB / SET_TEXTURE /
 * SET_COLORMAP_ID etc. before the batch.
 *
 * Fast path (default): builds the entire batch in an SDRAM scratch
 * buffer using cached scalar stores (~5 ns/word), flushes the populated
 * range, then writes one CMD_DRAW_SPANS_BATCH header into the ring and
 * kicks the GPU's doorbell-DMA puller.  GPU streams the buffer via its
 * own AXI master while the CPU is free to do other work.  Per-span
 * cost: 15 cached stores ≈ 75 ns vs 16 MMIO writes × 120 ns = 1920 ns
 * — about 25× faster, plus the CPU is no longer stalled on the AXI bus.
 *
 * Fallback (scratch malloc failed at init): emits N standalone
 * CMD_DRAW_SPAN commands via per-word MMIO.  Slower but correct.
 *
 * Caps the batch at OF_GPU_BATCH_MAX_SPANS spans per kick — longer
 * arrays split across multiple kicks.  No upper bound on the total
 * `count`; throughput is amortised. */
static inline void of_gpu_draw_spans_batch(const of_gpu_span_t *spans,
                                            int count) {
    if (count <= 0 || spans == NULL) return;

    if (_gpu_batch_buf == NULL) {
        /* Fallback: emit N standalone spans via MMIO. */
        for (int i = 0; i < count; i++)
            of_gpu_draw_span(&spans[i]);
        return;
    }

    while (count > 0) {
        int n = (count > OF_GPU_BATCH_MAX_SPANS) ? OF_GPU_BATCH_MAX_SPANS
                                                  : count;
        uint32_t payload_words = (uint32_t)(15 * n);

        /* Build the batch in cached SDRAM at scalar-store speed.  The
         * `wait for DMA at end of helper` policy below guarantees the
         * scratch buffer is free for reuse before we get here. */
        for (int i = 0; i < n; i++)
            _gpu_encode_span(&_gpu_batch_buf[i * 15], &spans[i]);

        /* Force the populated range out of L1 so the GPU's m_rd_*
         * AXI master sees committed bytes.  cbo.flush (writeback +
         * invalidate) is required on this VexiiRiscv config — cbo.clean
         * alone has been observed to leave dirty lines in L1, and the
         * GPU reads DRAM directly (not through the CPU's cache).
         * Same rationale as bank_preload's flush of the SF2 sample
         * blob before handing it to the audio mixer. */
        of_cache_flush_range(_gpu_batch_buf, payload_words * 4);

        /* Reserve enough ring space for header + DMA-streamed payload.
         * Write the header word via MMIO; this advances ring_wr_addr in
         * the BRAM but does NOT publish ring_wrptr — we let the DMA's
         * end-of-kick pulse publish header + payload atomically.  This
         * avoids a race where the decoder fetches the header and runs
         * into ring BRAM cells the DMA hasn't filled yet. */
        _gpu_ring_ensure((1 + payload_words) * 4);
        _gpu_ring_write(((uint32_t)GPU_CMD_DRAW_SPANS_BATCH << 24) |
                        payload_words);

        /* Arm and fire the DMA puller.  When DMA finishes the last
         * sub-burst, the GPU FSM lifts ring_wrptr to the post-DMA
         * ring_wr_addr — exposing the full command (header + payload)
         * to the decoder in a single atomic step. */
        GPU_DMA_SRC  = (uint32_t)(uintptr_t)_gpu_batch_buf;
        GPU_DMA_LEN  = payload_words;
        GPU_DMA_KICK = 1;

        /* Track the words the DMA will write — keep _gpu_wrptr in sync
         * with the GPU's eventual ring_wrptr so subsequent
         * _gpu_ring_ensure calls compute free space correctly. */
        _gpu_wrptr = (_gpu_wrptr + payload_words * 4) & _gpu_ring_mask;

        /* Wait for the DMA pull to complete before returning.  Critical:
         * the DMA shares ring_wr_addr with CPU MMIO writes (port-A mux
         * picks DMA when both fire same cycle).  If the caller issues
         * any ring write — fence in of_gpu_finish, SET_FB / SET_TEXTURE
         * for the next frame, etc. — while DMA is still streaming, the
         * CPU's data gets overwritten by DMA payload.  Drain takes
         * ~23 µs per max-size batch at 100 MHz; minor next to the ~100
         * µs of CPU compute typically waiting for the next batch.  The
         * GPU rasteriser already started consuming as DMA published
         * partial data — this wait is for the DMA fetch path, not for
         * the render itself, so CPU/GPU parallelism is preserved. */
        while (GPU_STATUS & GPU_STATUS_DMA_BUSY)
            ;

        spans += n;
        count -= n;
    }
}

static inline void _gpu_write_vertex(const of_gpu_vertex_t *v) {
    _gpu_ring_write(((uint32_t)(uint16_t)v->x << 16) | (uint16_t)v->y);
    _gpu_ring_write(((uint32_t)v->z << 16));
    _gpu_ring_write((uint32_t)v->s);
    _gpu_ring_write((uint32_t)v->t);
    _gpu_ring_write((uint32_t)v->w);
    _gpu_ring_write(((uint32_t)v->a << 24) | ((uint32_t)v->b << 16) |
                    ((uint32_t)v->g << 8) | v->r);
}

/*
 * Draw triangles from a vertex array.
 * @param verts        Every 3 consecutive vertices form one triangle.
 * @param num_vertices Number of vertices (must be a multiple of 3).
 *
 * Emits one CMD_DRAW_TRIANGLES per triangle (1 count word + 3 verts ×
 * 6 words = 19 payload words each).  Suitable when the surrounding
 * GPU state changes between triangles (e.g. per-triangle texture).
 */
static inline void of_gpu_draw_triangles(const of_gpu_vertex_t *verts,
                                          uint32_t num_vertices) {
    for (uint32_t i = 0; i < num_vertices; i += 3) {
        _gpu_cmd_header(GPU_CMD_DRAW_TRIANGLES, 19);
        _gpu_ring_write(3);
        _gpu_write_vertex(&verts[i + 0]);
        _gpu_write_vertex(&verts[i + 1]);
        _gpu_write_vertex(&verts[i + 2]);
    }
}

/*
 * Draw N triangles in a single batched DRAW_TRIANGLES command.
 *
 * Payload layout: 1 count word + N × 6 vertex words (6 words per vertex).
 * The GPU FSM renders each triangle as it streams in and re-enters the
 * payload loop for the next one, so the only difference from the
 * per-triangle helper above is one cmd_header + cmd_decode pass per
 * batch instead of per triangle.
 *
 * Constraint: every triangle in the batch shares the currently bound
 * texture and other GPU state.  Group triangles by state and submit
 * each group as one batch to amortise the command overhead.
 */
static inline void of_gpu_draw_triangles_batch(const of_gpu_vertex_t *verts,
                                                uint32_t num_vertices) {
    if (num_vertices < 3 || (num_vertices % 3) != 0) return;
    _gpu_cmd_header(GPU_CMD_DRAW_TRIANGLES, 1 + num_vertices * 6);
    _gpu_ring_write(num_vertices);
    for (uint32_t i = 0; i < num_vertices; i++)
        _gpu_write_vertex(&verts[i]);
}

#endif /* !OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_GPU_H */
