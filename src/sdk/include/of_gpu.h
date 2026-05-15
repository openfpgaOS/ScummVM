/*
 * of_gpu.h -- Hardware GPU Accelerator API for openfpgaOS
 *
 * Asynchronous span + triangle rasteriser.  CPU submits commands to a
 * 16 KB ring buffer in GPU-internal M10K BRAM; the GPU processes them
 * in parallel, writing pixels to the framebuffer via AXI4.
 *
 * Ring buffer: 16 KB in GPU-internal M10K BRAM.  CPU builds command
 * streams in a cached SDRAM scratch buffer, flushes and drains those
 * cache lines, then the GPU doorbell-DMA pulls the words into the ring
 * and publishes the write pointer atomically.  There is no CPU MMIO
 * command-data path.
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

#ifndef OF_PC
#include "of_caps.h"
#include "of_cache.h"
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
    /* Pixel count.  Scalar DRAW_SPAN's wire count is 12 bits at word 6
     * [27:16], so helpers mask this to 0..4095.  Callers that need
     * longer scalar spans should decompose them before submission. */
    uint16_t count;
    uint8_t  light;       /* low 6 bits select palookup shade row */
    uint8_t  flags;
    /* Word 6 [31:28]. Explicit palookup slot for this span, including 0. */
    uint8_t  colormap_id;
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
    uint32_t fb_addr;        /* framebuffer byte address for lane 0 */
    uint32_t tex_addr[8];    /* per-lane texture/column base */
    int32_t  t[8];           /* per-lane Q16.16 T coordinate */
    int32_t  tstep[8];       /* per-lane Q16.16 T step */
    uint16_t count;          /* pixels per lane */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by all lanes */
    uint8_t  colormap_id;    /* explicit slot, including slot 0 */
    uint8_t  lane_count;     /* API accepts 1..8; hardware chunks at 4 */
    int16_t  fb_stride;      /* row stride between vertical pixels */
    int16_t  lane_delta;     /* byte delta between adjacent lanes */
    uint16_t tex_width;      /* row pitch for tex_addr + t*tex_width */
    uint16_t tex_w_mask;     /* POT wrap mask for S, 0 means no wrap */
    uint16_t tex_h_mask;     /* POT wrap mask for T, 0 means no wrap */
    uint8_t  light[8];       /* per-lane palookup row, low 6 bits used */
} of_gpu_span_group_t;

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
    /* r = light index into the active palookup slot; low 6 bits select
     * the 64-row shade table.  Only `r` of v0
     * is sampled (flat shading per triangle).  CMD_DRAW_TRIANGLES
     * fragments ALWAYS route through palookup[colormap_id][r][texel];
     * any `r` value used MUST have its row populated, or the fragment
     * renders 0x00.  Convention: load row 0 with identity (cm[i]=i)
     * so r=0 means "raw textured / unlit". */
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
#define GPU_RING_WRPTR          OF_GPU_REG(0x04)  /* R: published write pointer */
#define GPU_DMA_SRC             OF_GPU_REG(0x0C)  /* W: SDRAM byte address of command buffer to pull */
#define GPU_RING_RDPTR          OF_GPU_REG(0x10)  /* R: GPU read pointer */
#define GPU_STATUS              OF_GPU_REG(0x14)  /* R: bit2=upload busy, bit1=ring empty, bit0=busy */
#define GPU_FENCE_REACHED       OF_GPU_REG(0x18)  /* R: last completed fence token */
#define GPU_DMA_LEN             OF_GPU_REG(0x1C)  /* W: word count to pull (≤4096) */
#define GPU_TRANSLUC_ADDR       OF_GPU_REG(0x20)  /* W: byte addr into transluc[] (auto-inc by 4) */
#define GPU_TRANSLUC_DATA       OF_GPU_REG(0x24)  /* W: 32-bit word into transluc[] */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */
#define GPU_DMA_KICK            OF_GPU_REG(0x2C)  /* W: write 1 to fire DMA pull from (SRC, LEN) */

/* GPU_STATUS bit definitions */
#define GPU_STATUS_BUSY        0x1u
#define GPU_STATUS_RING_EMPTY  0x2u
#define GPU_STATUS_DMA_BUSY    0x4u  /* SDRAM command/payload DMA busy */

/* ================================================================
 * Command IDs
 * ================================================================ */

#define GPU_CMD_FENCE           0x02
#define GPU_CMD_CLEAR_RECT      0x11  /* 3-word payload: start byte addr,
                                       * {w,h}, {pad,color}. Color's low
                                       * byte is replicated 4× per FB word. */
#define GPU_CMD_SET_TEXTURE     0x20
/* 0x21 GPU_CMD_SET_DEPTH_FUNC retired in lean Phase 2.3 (Z dropped). */
#define GPU_CMD_SET_FB          0x23
/* 0x24 GPU_CMD_SET_ZB         retired in lean Phase 2.3 (Z dropped). */
#define GPU_CMD_DRAW_TRIANGLES  0x30
/* 0x40 GPU_CMD_DRAW_SPAN retired: scalar spans now use
 * GPU_CMD_DRAW_SPAN_GROUP with the 15-word scalar payload. */
/* 0x41 GPU_CMD_DRAW_SPANS_BATCH retired: software batches ordinary
 * span commands into one command stream instead of using a second
 * scalar-span decoder in RTL. */
/* GPU-triggered display flip (cr-gpu-triggered-flip.md).  2-word payload:
 *   word 0: bits[1:0] = back-buffer index (0/1/2 → FB_ADDR_{0,1,2})
 *   word 1: fence token (published to GPU_FENCE_REACHED after the swap)
 * Drains all outstanding m_wr_* writes (same primitive as the upgraded
 * CMD_FENCE), pulses the swap side-port to axi_periph_slave for one
 * cycle, then publishes the fence token. */
#define GPU_CMD_FLIP             0x42
#define GPU_CMD_DRAW_SPAN_GROUP       0x43  /* 15-word scalar or 18-word group */

/* Maximum spans per scalar-span stream chunk.  Each span emits a normal
 * command header plus 15 payload words; longer arrays split across kicks. */
#define OF_GPU_BATCH_MAX_SPANS  128
#define OF_GPU_SPAN_GROUP_WORDS      18u

/* ================================================================
 * Palookup (colormap) layout in SDRAM — must match gpu_core.v's
 * PALOOKUP_BASE / PALOOKUP_STRIDE constants.
 *
 * Each slot holds a Quake/BUILD-shape shade × texel table.  Scalar spans
 * and span groups carry an explicit colormap_id in each command. Triangles
 * currently use slot 0. Up to 16 slots; the GPU reads
 * palookup[slot][shade & 63][texel] from
 *   GPU_AXI_BASE + OF_GPU_PALOOKUP_AXI_OFFSET
 *              + slot*0x4000 + shade*256 + texel
 * via gpu_tex_cache port B (the prior on-chip cmap_bram is retired).
 *
 * The CPU-visible address depends on how the target maps the GPU's
 * AXI M0 into the CPU address space — apps should obtain it via the
 * runtime caps descriptor and add the per-slot offset.  These
 * constants encode the GPU-side AXI offset and per-slot
 * stride; the kernel's caps descriptor adds the per-target physical
 * base.  The lookup is target-portable as long as the kernel
 * advertises a `palookup_base` field that maps the same 26-bit
 * GPU AXI offset.
 * ================================================================ */
/* GPU AXI M0 byte addr of palookup slot 0.  Was 0x00100000 but that
 * collided with OF_TARGET_FB1_BASE = 0x10100000, so every FB1 frame
 * overwrote the palookup table.  Moved to the 3 MB gap between heap
 * end (0x13400000) and the audio sample pool (0x13700000).  MUST stay
 * in sync with PALOOKUP_BASE in src/fpga/common/gpu_core.v. */
#define OF_GPU_PALOOKUP_AXI_OFFSET 0x03400000u
#define OF_GPU_PALOOKUP_STRIDE     0x00004000u  /* 16 KB per slot */
#define OF_GPU_PALOOKUP_SLOTS      16

/* Doorbell-DMA scratch region — must live in SDRAM because gpu_core's
 * m_rd_* AXI master only reaches the SDRAM arbiter (see core_top.v's
 * sdram_arb instantiation: GPU is m0, no other targets are wired).
 * CPU writes this window through the cached alias for speed.  Before each
 * GPU DMA kick, of_gpu drains the flushed cache lines with same-master
 * readbacks so the GPU cannot read stale command words. */
#define OF_GPU_BATCH_BUF_AXI_OFFSET  0x00140000u
#define OF_GPU_BATCH_BUF_BYTES       0x00004000u  /* 16 KB reserved */
#define OF_GPU_CACHE_LINE_BYTES      64u

/* ================================================================
 * Ring Buffer State (app-side)
 *
 * Static mutable — include this header from one .c file only.
 * ================================================================ */

static uint32_t _gpu_wrptr;
static uint32_t _gpu_known_rdptr;
static uint32_t _gpu_fence_next;
static uint32_t _gpu_cmd_words;
static uint32_t _gpu_batch_dma_addr;

static const uint32_t _gpu_ring_mask = OF_GPU_RING_SIZE - 1;

/* Doorbell-DMA scratch buffer.  Pinned to a fixed SDRAM offset by
 * of_gpu_init and kept cached so command construction does not stall on
 * every store.  NULL on targets that don't expose SDRAM; command
 * submission traps on those targets because the legacy MMIO command path
 * is retired. */
static uint32_t *_gpu_batch_buf;
static uint32_t  _gpu_dbg_dma_waits;
static uint32_t  _gpu_dbg_dma_spin_iters;
static uint32_t  _gpu_dbg_ring_waits;
static uint32_t  _gpu_dbg_ring_spin_iters;
static uint32_t  _gpu_dbg_min_ring_free;

static uint32_t _gpu_state_valid;
static uint32_t _gpu_state_fb_addr;
static uint32_t _gpu_state_fb_stride;
static uint32_t _gpu_state_tex_addr;
static uint32_t _gpu_state_tex_dims;

#define OF_GPU_STATE_FB       (1u << 0)
#define OF_GPU_STATE_TEXTURE  (1u << 1)

#define OF_GPU_BATCH_WORDS_PER_SPAN   15u
#define OF_GPU_COMMAND_STREAM_BATCH_WORDS ((OF_GPU_RING_SIZE / 4u) - 1u)

#if ((OF_GPU_BATCH_MAX_SPANS * (1u + OF_GPU_BATCH_WORDS_PER_SPAN)) > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
#error "OF_GPU_COMMAND_STREAM_BATCH_WORDS too small for scalar span batch"
#endif

#if (OF_GPU_COMMAND_STREAM_BATCH_WORDS * 4u) > OF_GPU_BATCH_BUF_BYTES
#error "GPU command stream buffer must fit in the reserved SDRAM scratch"
#endif

/* ---- Internal helpers ---- */

static inline void _gpu_wait_dma_idle_debug(void) {
    uint32_t dma_spins = 0;
    while (GPU_STATUS & GPU_STATUS_DMA_BUSY)
        dma_spins++;
    if (dma_spins) {
        _gpu_dbg_dma_waits++;
        _gpu_dbg_dma_spin_iters += dma_spins;
    }
}

static inline uint32_t _gpu_ring_free_now(void) {
    uint32_t rdptr = GPU_RING_RDPTR;
    _gpu_known_rdptr = rdptr;
    return (rdptr - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline uint32_t _gpu_ring_free_known(void) {
    return (_gpu_known_rdptr - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline void _gpu_note_ring_free(uint32_t ring_free) {
    if (ring_free < _gpu_dbg_min_ring_free)
        _gpu_dbg_min_ring_free = ring_free;
}

static inline void _gpu_cbo_flush_line(void *addr) {
    __asm__ volatile(".insn i 0x0F, 2, x0, %0, 2" :: "r"(addr) : "memory");
}

static inline void _gpu_flush_cmd_cache_range(void *addr, uint32_t bytes) {
    __asm__ volatile("fence" ::: "memory");
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(OF_GPU_CACHE_LINE_BYTES - 1u);
    uintptr_t end = (uintptr_t)addr + bytes;
    for (; a < end; a += OF_GPU_CACHE_LINE_BYTES)
        _gpu_cbo_flush_line((void *)a);
    __asm__ volatile("fence" ::: "memory");
}

static inline void _gpu_drain_cmd_writeback(uint32_t bytes) {
    if (bytes == 0)
        return;

    volatile const uint32_t *p = (volatile const uint32_t *)_gpu_batch_buf;
    uint32_t words = (bytes + 3u) >> 2;
    uint32_t sink = 0;

    /* of_cache_flush_range() invalidates each line after scheduling its
     * writeback.  These cached reads use the same d_axi master as those
     * writebacks, unlike a p_axi uncached read, so they force the CPU-side
     * memory stream to observe the flushed command data before GPU DMA
     * gets priority on the SDRAM arbiter. */
    for (uint32_t off = 0; off < bytes; off += OF_GPU_CACHE_LINE_BYTES)
        sink ^= p[off >> 2];
    sink ^= p[words - 1u];

    __asm__ volatile("" :: "r"(sink) : "memory");
    __asm__ volatile("fence" ::: "memory");
}

static inline void _gpu_flush_cmd_stream(void) {
    if (_gpu_cmd_words == 0)
        return;
    if (_gpu_batch_buf == NULL)
        __builtin_trap();

    _gpu_wait_dma_idle_debug();

    _gpu_flush_cmd_cache_range(_gpu_batch_buf, _gpu_cmd_words * 4u);
    _gpu_drain_cmd_writeback(_gpu_cmd_words * 4u);

    __asm__ volatile("fence" ::: "memory");
    GPU_DMA_SRC  = _gpu_batch_dma_addr;
    GPU_DMA_LEN  = _gpu_cmd_words;
    __asm__ volatile("fence" ::: "memory");
    GPU_DMA_KICK = 1;
    __asm__ volatile("fence" ::: "memory");
    _gpu_cmd_words = 0;
}

static inline void _gpu_ring_ensure(uint32_t bytes) {
    if (bytes > (OF_GPU_RING_SIZE - 4u))
        __builtin_trap();

    uint32_t ring_free = _gpu_ring_free_known();
    _gpu_note_ring_free(ring_free);
    if (ring_free >= bytes)
        return;

    /* Slow path: if commands are only staged in SDRAM, first publish them
     * through the doorbell DMA so the GPU has something to drain. */
    _gpu_flush_cmd_stream();
    _gpu_wait_dma_idle_debug();

    {
        uint32_t ring_spins = 0;
        do {
            ring_free = _gpu_ring_free_now();
            _gpu_note_ring_free(ring_free);
            if (ring_free >= bytes)
                break;
            ring_spins++;
        } while (1);
        _gpu_dbg_ring_waits++;
        _gpu_dbg_ring_spin_iters += ring_spins;
    }
}

static inline void _gpu_stream_reserve_words(uint32_t words) {
    if (words == 0)
        return;
    if (_gpu_batch_buf == NULL || words > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();
    if (_gpu_cmd_words + words > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        _gpu_flush_cmd_stream();
    _gpu_ring_ensure(words * 4u);
}

/* Append one word to the staged SDRAM command stream.  Callers reserve
 * a whole command first so this helper never flushes in the middle of a
 * command payload. */
static inline void _gpu_ring_write(uint32_t w) {
    if (_gpu_batch_buf == NULL || _gpu_cmd_words >= OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();
    _gpu_batch_buf[_gpu_cmd_words++] = w;
    _gpu_wrptr = (_gpu_wrptr + 4) & _gpu_ring_mask;
}

static inline void _gpu_cmd_header(uint8_t cmd, uint32_t payload_words) {
    _gpu_stream_reserve_words(1u + payload_words);
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
    _gpu_known_rdptr = 0;
    _gpu_fence_next = 1;
    _gpu_cmd_words = 0;
    _gpu_batch_dma_addr = 0;
    _gpu_dbg_dma_waits = 0;
    _gpu_dbg_dma_spin_iters = 0;
    _gpu_dbg_ring_waits = 0;
    _gpu_dbg_ring_spin_iters = 0;
    _gpu_dbg_min_ring_free = OF_GPU_RING_SIZE;
    _gpu_state_valid = 0;
    GPU_CTRL = 4;               /* ring_reset: clear wr_addr + wrptr + rdptr */
    GPU_CTRL = 1;               /* enable */

    /* Pin the doorbell-DMA scratch buffer at a known SDRAM offset.
     * Command words are written through the cached alias for normal CPU
     * store speed.  _gpu_flush_cmd_stream() handles the external-master
     * handoff by flushing and then reading back the invalidated lines on
     * the same d_axi path before GPU_DMA_KICK. */
    {
        const struct of_capabilities *caps = of_get_caps();
        if (caps && caps->sdram_base != 0) {
            _gpu_batch_dma_addr = caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET;
            _gpu_batch_buf = (uint32_t *)(uintptr_t)
                (caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET);
        } else {
            _gpu_batch_buf = NULL;   /* command submission will trap */
        }
    }
}

/* Upload a palookup table to slot N in SDRAM.  The GPU reads palookup
 * bytes through gpu_tex_cache port B; this helper writes the table
 * directly through the uncached SDRAM alias so the GPU sees committed
 * data.  16 KB per slot, up to 16 slots (cf. OF_GPU_PALOOKUP_*).
 *
 * Span and span-group draw commands select slots explicitly with their
 * colormap_id fields. Triangle draws use slot 0, so apps that submit
 * triangles should upload row 0 as identity when they want raw texels. */
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
     * Use 32-bit word writes, not single-byte writes.  Pocket's SDRAM
     * controller passes wstrb through, but the 16-bit PHY behind it
     * resolves byte-strobed writes via a read-modify-write path that
     * has produced "palookup mostly zeros" symptoms in practice — the
     * GPU then sees a near-uniform table and every pixel resolves to
     * the same palette index regardless of (texel, shade), which reads
     * on screen as a uniform colour with no fade.  Full-word writes
     * sidestep the RMW path; the SDRAM slave's [25:2] address decode
     * means consecutive uint32_t writes hit consecutive SDRAM words.
     *
     * The prior cached + cache_clean version had the same class of
     * stale-data bug; uncached alias is the right destination, just
     * in 32-bit chunks rather than bytes. */
    uint32_t cached_base = caps->sdram_base
                         + OF_GPU_PALOOKUP_AXI_OFFSET
                         + (uint32_t)slot * OF_GPU_PALOOKUP_STRIDE;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)
        ((cached_base - caps->sdram_base) + caps->sdram_uncached_base);
    const uint8_t *src = data;
    uint32_t words = size >> 2;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t off = i << 2;
        dst[i] = (uint32_t)src[off + 0] |
                 ((uint32_t)src[off + 1] << 8) |
                 ((uint32_t)src[off + 2] << 16) |
                 ((uint32_t)src[off + 3] << 24);
    }
    /* Tail bytes (size not a multiple of 4) — fold into a final word
     * so we still write every byte the caller passed.  Pad the unused
     * lanes with zero rather than skipping, so the SDRAM word is
     * fully defined regardless of whatever was there before. */
    uint32_t tail = size & 3u;
    if (tail) {
        uint32_t w = 0;
        const uint8_t *tb = src + (words << 2);
        for (uint32_t i = 0; i < tail; i++)
            w |= ((uint32_t)tb[i]) << (i * 8);
        dst[words] = w;
    }
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
    _gpu_flush_cmd_stream();
}

static inline uint32_t of_gpu_fence(void) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FENCE, 1);
    _gpu_ring_write(token);
    return token;
}

/* GPU-triggered page flip — emits CMD_FLIP into the ring with the
 * given back-buffer index and a fresh fence token.  The GPU's command
 * processor drains all outstanding m_wr_* writes, pulses the swap
 * side-port to the display controller (queued for next vsync), and
 * publishes the fence token to GPU_FENCE_REACHED.  Non-blocking — the
 * returned token proves the swap request reached the display
 * controller, not that the next vsync has presented it.
 *
 * Pair with the kernel's of_video_acquire_next(idx) to get the next
 * free draw buffer.  See docs/cr-gpu-triggered-flip.md for the
 * standard call pattern. */
/* CMD_FLIP re-enabled with diagnostic counters in place (2026-04-30).
 * The kernel side of_video_acquire_next() retains a bounded fence-wait
 * and only uses a CPU FB_SWAP_CTRL write as a timeout fallback, so a
 * healthy CMD_FLIP path stays non-blocking and does not double-kick
 * the same swap. */
static inline uint32_t of_gpu_flip_to(int idx) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FLIP, 2);
    _gpu_ring_write((uint32_t)idx & 0x3u);
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
    _gpu_state_valid = 0;
}

typedef struct {
    uint32_t status;
    uint32_t rdptr;
    uint32_t wrptr;
    uint32_t fence_reached;
    uint32_t dma_waits;
    uint32_t dma_spin_iters;
    uint32_t ring_waits;
    uint32_t ring_spin_iters;
    uint32_t min_ring_free;
    uint32_t ring_free;
} of_gpu_debug_snapshot_t;

static inline void of_gpu_debug_snapshot(of_gpu_debug_snapshot_t *snap,
                                         int reset_wait_counters) {
    if (snap == NULL)
        return;

    memset(snap, 0, sizeof(*snap));
    snap->status = GPU_STATUS;
    snap->rdptr = GPU_RING_RDPTR;
    snap->wrptr = _gpu_wrptr;
    snap->fence_reached = GPU_FENCE_REACHED;

    snap->ring_free = _gpu_ring_free_now();
    snap->min_ring_free = _gpu_dbg_min_ring_free < snap->ring_free ?
        _gpu_dbg_min_ring_free : snap->ring_free;
    snap->dma_waits = _gpu_dbg_dma_waits;
    snap->dma_spin_iters = _gpu_dbg_dma_spin_iters;
    snap->ring_waits = _gpu_dbg_ring_waits;
    snap->ring_spin_iters = _gpu_dbg_ring_spin_iters;

    if (reset_wait_counters) {
        _gpu_dbg_dma_waits = 0;
        _gpu_dbg_dma_spin_iters = 0;
        _gpu_dbg_ring_waits = 0;
        _gpu_dbg_ring_spin_iters = 0;
        _gpu_dbg_min_ring_free = snap->ring_free;
    }
}

/* ---- State commands ---- */

static inline void of_gpu_set_framebuffer(uint32_t addr, uint16_t stride) {
    if ((_gpu_state_valid & OF_GPU_STATE_FB) &&
        _gpu_state_fb_addr == addr &&
        _gpu_state_fb_stride == (uint32_t)stride)
        return;

    _gpu_cmd_header(GPU_CMD_SET_FB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
    _gpu_state_fb_addr = addr;
    _gpu_state_fb_stride = (uint32_t)stride;
    _gpu_state_valid |= OF_GPU_STATE_FB;
}

/* Z-buffer / depth-test API retired with the lean Z-removal in Phase 2.3.
 * Quake / SDL2 / Doom-style renderers do their own visibility (BSP /
 * paint-order); the GPU is now strictly a paint-order rasterizer. */

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    uint32_t dims = ((uint32_t)tex->width << 16) | tex->height;
    if ((_gpu_state_valid & OF_GPU_STATE_TEXTURE) &&
        _gpu_state_tex_addr == tex->addr &&
        _gpu_state_tex_dims == dims)
        return;

    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 2);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(dims);
    _gpu_state_tex_addr = tex->addr;
    _gpu_state_tex_dims = dims;
    _gpu_state_valid |= OF_GPU_STATE_TEXTURE;
}

/* ---- Draw commands ---- */

/* Whole-FB clear.  flags bit 0 = clear color (the only flag still
 * accepted; the bit-1 depth-clear path was retired with the Z buffer). */
static inline void of_gpu_clear(uint32_t flags, uint16_t color) {
    if ((flags & OF_GPU_CLEAR_COLOR) == 0)
        return;

    /* The old whole-FB clear was a fixed 320x200 contiguous write from
     * the current framebuffer base.  Keep that public behavior but encode
     * it as a normal rectangle clear so the RTL only has one clear path. */
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(_gpu_state_fb_addr);
    _gpu_ring_write((320u << 16) | 200u);
    _gpu_ring_write((320u << 16) | ((uint32_t)color & 0xFFu));
}

/* Clear a rectangular region of the framebuffer to a constant color.
 * Caller computes the start byte address (fb_base + y*stride + x); the
 * GPU walks `h` rows × `w` bytes from there, advancing each row by the
 * active st_fb_stride.  Color's low byte is replicated 4× per FB word.
 * Word-aligned full-width strips
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

/* Strided clear_rect — word 2 of the payload carries the row stride at
 * bits [31:16].  When stride==0 the GPU falls back to the SET_FB-
 * resident global stride (matches plain of_gpu_clear_rect).  See
 * docs/cr-gpu-clear-rect-stride.md for the rationale. */
static inline void of_gpu_clear_rect_strided(uint32_t start_byte_addr,
                                              uint16_t w, uint16_t h,
                                              uint16_t stride,
                                              uint8_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(start_byte_addr);
    _gpu_ring_write(((uint32_t)w << 16) | (uint32_t)h);
    _gpu_ring_write(((uint32_t)stride << 16) | (uint32_t)color);
}

/*
 * Draw a single span.  15 payload words: 9 core + 6 perspective.
 * GPU ignores the perspective words unless OF_GPU_SPAN_PERSP is set.
 */
static inline void of_gpu_draw_span(const of_gpu_span_t *span) {
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN_GROUP, 15);
    _gpu_ring_write(span->fb_addr);
    _gpu_ring_write(span->tex_addr);
    _gpu_ring_write((uint32_t)span->s);
    _gpu_ring_write((uint32_t)span->t);
    _gpu_ring_write((uint32_t)span->sstep);
    _gpu_ring_write((uint32_t)span->tstep);
    _gpu_ring_write((((uint32_t)span->colormap_id & 0xFu) << 28) |
                    (((uint32_t)span->count & 0x0FFFu) << 16) |
                    (((uint32_t)span->light & 0x3Fu) << 8) |
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

/* Encode one compact affine span group.  The hardware walks lanes row-first
 * through the same fragment path as scalar spans:
 *   fb_addr + lane_delta*lane selects each destination lane
 *   fb_stride advances to the next row after the last lane
 *   tex_addr/t/tstep/light are independent per lane
 *
 * The native command is capped at four lanes because a group maps to one
 * 32-bit framebuffer word.  The public API still accepts up to eight lanes;
 * of_gpu_draw_span_group() splits wider requests into multiple native
 * groups. */
static inline void _gpu_encode_span_group_chunk(uint32_t *p,
                                                const of_gpu_span_group_t *s,
                                                uint32_t first_lane,
                                                uint32_t lane_count) {
    uint32_t fb_off = (uint32_t)((int32_t)s->lane_delta * (int32_t)first_lane);
    p[0] = s->fb_addr + fb_off;
    p[1] = ((uint32_t)s->count << 16) |
           ((uint32_t)s->flags << 8) |
           ((lane_count & 0x0Fu) << 4) |
           ((uint32_t)s->colormap_id & 0x0Fu);
    p[2] = ((uint32_t)(uint16_t)s->fb_stride << 16) |
           (uint32_t)(uint16_t)s->lane_delta;
    p[3] = (uint32_t)s->tex_width;
    p[4] = ((uint32_t)s->tex_h_mask << 16) |
           (uint32_t)s->tex_w_mask;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t src = first_lane + i;
        p[5u + i]  = (i < lane_count) ? s->tex_addr[src] : 0;
        p[9u + i]  = (i < lane_count) ? (uint32_t)s->t[src] : 0;
        p[13u + i] = (i < lane_count) ? (uint32_t)s->tstep[src] : 0;
    }
    p[17] = (((uint32_t)((lane_count > 3) ? s->light[first_lane + 3u] : 0) & 0x3Fu) << 24) |
            (((uint32_t)((lane_count > 2) ? s->light[first_lane + 2u] : 0) & 0x3Fu) << 16) |
            (((uint32_t)((lane_count > 1) ? s->light[first_lane + 1u] : 0) & 0x3Fu) << 8) |
            ((uint32_t)((lane_count > 0) ? s->light[first_lane] : 0) & 0x3Fu);
}

static inline void of_gpu_draw_span_group(const of_gpu_span_group_t *span) {
    uint32_t w[OF_GPU_SPAN_GROUP_WORDS];
    if (span == NULL) return;

    uint32_t lanes_left = span->lane_count;
    if (lanes_left == 0)
        return;
    if (lanes_left > 8u)
        lanes_left = 8u;

    for (uint32_t first = 0; lanes_left != 0;) {
        uint32_t n = (lanes_left >= 4u) ? 4u :
                     (lanes_left >= 2u) ? 2u : 1u;
        _gpu_encode_span_group_chunk(w, span, first, n);
        _gpu_cmd_header(GPU_CMD_DRAW_SPAN_GROUP, OF_GPU_SPAN_GROUP_WORDS);
        for (uint32_t i = 0; i < OF_GPU_SPAN_GROUP_WORDS; i++)
            _gpu_ring_write(w[i]);
        first += n;
        lanes_left -= n;
    }
}

static inline void of_gpu_draw_span_group_batch(const of_gpu_span_group_t *spans,
                                                int count) {
    if (count <= 0 || spans == NULL) return;

    for (int i = 0; i < count; i++)
        of_gpu_draw_span_group(&spans[i]);
    _gpu_flush_cmd_stream();
}

/* Submit an already-encoded command stream through the doorbell-DMA path.
 *
 * `words` must contain complete GPU commands, including each command
 * header.  This can batch order-sensitive mixtures of scalar and compact
 * span payloads without flushing whenever descriptor type changes.
 *
 * The helper does not split the stream because splitting inside a
 * command would publish an incomplete command to the decoder.  Callers
 * must cap each stream to OF_GPU_COMMAND_STREAM_BATCH_WORDS and flush
 * only at command boundaries. */
static inline void of_gpu_submit_command_stream_batch(const uint32_t *words,
                                                       int word_count) {
    if (word_count <= 0 || words == NULL) return;
    if ((uint32_t)word_count > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();

    uint32_t stream_words = (uint32_t)word_count;
    _gpu_stream_reserve_words(stream_words);

    for (int i = 0; i < word_count; i++)
        _gpu_batch_buf[_gpu_cmd_words + (uint32_t)i] = words[i];

    _gpu_cmd_words += stream_words;
    _gpu_wrptr = (_gpu_wrptr + stream_words * 4u) & _gpu_ring_mask;
    _gpu_state_valid = 0;
    _gpu_flush_cmd_stream();
}

/* of_gpu_draw_spans_batch — submit N spans in one go.
 *
 * Same per-span semantics as of_gpu_draw_span.  The helper emits scalar
 * span payloads into the cached command stream and flushes at chunk
 * boundaries. */
static inline void of_gpu_draw_spans_batch(const of_gpu_span_t *spans,
                                            int count) {
    if (count <= 0 || spans == NULL) return;

    while (count > 0) {
        int n = (count > OF_GPU_BATCH_MAX_SPANS) ? OF_GPU_BATCH_MAX_SPANS
                                                  : count;
        for (int i = 0; i < n; i++)
            of_gpu_draw_span(&spans[i]);
        _gpu_flush_cmd_stream();

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
                    ((uint32_t)v->g << 8) | (v->r & 0x3Fu));
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
