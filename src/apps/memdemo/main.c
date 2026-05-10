/*
 * openfpgaOS Memory Performance Demo
 *
 * Benchmarks memset, memcpy, random access at various buffer sizes
 * to characterize throughput and cache behavior across SDRAM, PSRAM, and BRAM.
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Aligned source buffer in SDRAM (used as memcpy source) */
static uint8_t src_buf[1024 * 1024] __attribute__((aligned(64)));
/* SDRAM destination buffer */
static uint8_t dst_buf[1024 * 1024] __attribute__((aligned(64)));

/* Prevent the compiler from optimizing away the operations */
static volatile uint8_t sink;

/* CRAM0 is i_axi-only under the current PMA (see
 * GenOpenFpgaVexii.scala — CRAM0 excluded from d_axi so the sync-burst
 * refill path stays single-master).  All data-plane access to CRAM0
 * must use the 0x38xxxxxx uncached alias, which routes through p_axi.
 * Cached stores to 0x30xxxxxx trap with store-access-fault.
 *
 * The OS kernel occupies 0x30000000-0x30100000; we test at 0x38200000
 * to leave 1MB headroom for the kernel and any app PIE fallback. */
#define PSRAM_BASE  0x38200000   /* uncached alias — cached is forbidden */
/* Uncached SDRAM mirror — bypass D-cache for raw bus throughput measurement.
 * PSRAM no longer has a separate cached/uncached distinction at the CPU
 * level (PMA-forbidden), so UNCACHED_PSRAM_OFF is 0 and the srd/u rows
 * measure the same path as seq_rd. */
#define UNCACHED_SDRAM_OFF  0x40000000
#define UNCACHED_PSRAM_OFF  0           /* PSRAM is always uncached now */

/* Test sizes: 256, 1K, 16K, 256K */
static const uint32_t sizes[]  = { 256, 1024, 16384, 262144 };
static const int      reps[]   = { 1000, 1000, 200, 10 };
#define NUM_SIZES 4

static void fmt_mbps(char *out, int len, uint32_t size, uint32_t us, int r) {
    uint32_t total = size * (uint32_t)r;
    uint32_t safe = us > 0 ? us : 1;
    uint32_t x10 = (uint64_t)total * 10 / safe;
    snprintf(out, len, "%3u.%u", x10 / 10, x10 % 10);
}

/* Simple PRNG for random access (xorshift32) */
static uint32_t xor_state = 0x12345678;
static inline uint32_t xorshift32(void) {
    uint32_t x = xor_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xor_state = x;
    return x;
}

/* ---- Benchmark primitives ---- */

static uint32_t bench_seq_read(const void *buf, uint32_t size, int r) {
    volatile uint32_t *p = (volatile uint32_t *)buf;
    uint32_t n_words = size / 4;
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++) {
        uint32_t acc = 0;
        for (uint32_t j = 0; j < n_words; j++)
            acc += p[j];
        sink = (uint8_t)acc;
    }
    uint32_t t1 = of_time_us();
    return t1 - t0;
}

static uint32_t bench_seq_write(void *buf, uint32_t size, int r) {
    volatile uint32_t *p = (volatile uint32_t *)buf;
    uint32_t n_words = size / 4;
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++) {
        for (uint32_t j = 0; j < n_words; j++)
            p[j] = j;
    }
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)buf;
    return t1 - t0;
}

static uint32_t bench_memset(void *dst, uint32_t size, int r) {
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++)
        memset(dst, i & 0xFF, size);
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)dst;
    return t1 - t0;
}

static uint32_t bench_memcpy(void *dst, const void *src, uint32_t size, int r) {
    memset((void *)src, 0xAA, size);
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++)
        memcpy(dst, src, size);
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)dst;
    return t1 - t0;
}

static uint32_t bench_random(void *buf, uint32_t size, int r, void *idx_store) {
    volatile uint32_t *p = (volatile uint32_t *)buf;
    uint32_t n_words = size / 4;
    /* Pre-calculate random indices into separate memory region */
    uint32_t *idx_tbl = (uint32_t *)idx_store;
    xor_state = 0x12345678;
    for (uint32_t j = 0; j < n_words; j++)
        idx_tbl[j] = xorshift32() % n_words;
    uint32_t t0 = of_time_us();
    for (int i = 0; i < r; i++) {
        for (uint32_t j = 0; j < n_words; j++)
            p[idx_tbl[j]] = j;
    }
    uint32_t t1 = of_time_us();
    sink = *(volatile uint8_t *)buf;
    return t1 - t0;
}

/* ---- Output helpers ---- */

#define SEP "---------------------------------------"

static void print_header(const char *label) {
    printf("\n%-8s %5s   %5s   %5s   %5s\n", label, "256", "1K", "16K", "256K");
    printf(SEP "\n");
}

static void print_row(const char *name, char results[][16], int n) {
    printf("%-8s", name);
    for (int i = 0; i < n; i++)
        printf(i < n - 1 ? "%7s " : "%7s", results[i]);
    printf("\n");
}

/* ---- Per-region suites ---- */

static void run_sdram(void) {
    void *dst = dst_buf;
    void *src = src_buf;
    char r[NUM_SIZES][16];

    print_header("SDRAM");

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_memset(dst, sizes[i], reps[i]), reps[i]);
    print_row("memset", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_memcpy(dst, src, sizes[i], reps[i]), reps[i]);
    print_row("memcpy", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_read(dst, sizes[i], reps[i]), reps[i]);
    print_row("seq_rd", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_write(dst, sizes[i], reps[i]), reps[i]);
    print_row("seq_wr", r, NUM_SIZES);

    void *udst = (void *)((uintptr_t)dst + UNCACHED_SDRAM_OFF);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_read(udst, sizes[i], reps[i]), reps[i]);
    print_row("srd/u", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_write(udst, sizes[i], reps[i]), reps[i]);
    print_row("swr/u", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_random(dst, sizes[i], reps[i], (void *)PSRAM_BASE), reps[i]);
    print_row("random", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_random(udst, sizes[i], reps[i], (void *)PSRAM_BASE), reps[i]);
    print_row("rand/u", r, NUM_SIZES);

}

static void run_psram(void) {
    void *dst = (void *)PSRAM_BASE;
    void *src = (void *)(PSRAM_BASE + 1024 * 1024);
    char r[NUM_SIZES][16];

    print_header("PSRAM");

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_memset(dst, sizes[i], reps[i]), reps[i]);
    print_row("memset", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_memcpy(dst, src, sizes[i], reps[i]), reps[i]);
    print_row("memcpy", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_read(dst, sizes[i], reps[i]), reps[i]);
    print_row("seq_rd", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_write(dst, sizes[i], reps[i]), reps[i]);
    print_row("seq_wr", r, NUM_SIZES);

    void *udst = (void *)((uintptr_t)dst + UNCACHED_PSRAM_OFF);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_read(udst, sizes[i], reps[i]), reps[i]);
    print_row("srd/u", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_seq_write(udst, sizes[i], reps[i]), reps[i]);
    print_row("swr/u", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_random(dst, sizes[i], reps[i], src_buf), reps[i]);
    print_row("random", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++)
        fmt_mbps(r[i], 16, sizes[i], bench_random(udst, sizes[i], reps[i], src_buf), reps[i]);
    print_row("rand/u", r, NUM_SIZES);
}

// run_sram removed — SRAM is GPU-exclusive (Z-buffer)

static void run_bram(void) {
    void *dst = (void *)0x00002000;
    void *src = (void *)0x00004C00;
    /* BRAM app window is 22KB [0x2000,0x7800). Two non-overlapping buffers
     * of up to 11KB each: dst [0x2000,0x4C00), src [0x4C00,0x7800).
     * 16KB test dropped — src+16K aliases past 0x8000 back to 0x0000,
     * overwriting the trap handler. */
    static const uint32_t bram_sizes[] = { 256, 1024, 8192 };
    static const int      bram_reps[]  = { 1000, 1000, 200 };
    #define NUM_BRAM 3
    char r[NUM_SIZES][16];

    print_header("BRAM");

    for (int i = 0; i < NUM_BRAM; i++)
        fmt_mbps(r[i], 16, bram_sizes[i], bench_memset(dst, bram_sizes[i], bram_reps[i]), bram_reps[i]);
    snprintf(r[3], 16, "  ---");
    print_row("memset", r, NUM_SIZES);

    for (int i = 0; i < NUM_BRAM; i++)
        fmt_mbps(r[i], 16, bram_sizes[i], bench_memcpy(dst, src, bram_sizes[i], bram_reps[i]), bram_reps[i]);
    snprintf(r[3], 16, "  ---");
    print_row("memcpy", r, NUM_SIZES);

    for (int i = 0; i < NUM_BRAM; i++)
        fmt_mbps(r[i], 16, bram_sizes[i], bench_random(dst, bram_sizes[i], bram_reps[i], src_buf), bram_reps[i]);
    snprintf(r[3], 16, "  ---");
    print_row("random", r, NUM_SIZES);
}

static void run_cross(void) {
    void *sdram = dst_buf;
    void *psram = (void *)PSRAM_BASE;
    char r[NUM_SIZES][16];

    /* P->S (PSRAM to SDRAM) */
    print_header("P->S");

    for (int i = 0; i < NUM_SIZES; i++) {
        memset(psram, 0xBB, sizes[i]);
        uint32_t t0 = of_time_us();
        for (int j = 0; j < reps[i]; j++)
            memcpy(sdram, psram, sizes[i]);
        uint32_t t1 = of_time_us();
        sink = *(volatile uint8_t *)sdram;
        fmt_mbps(r[i], 16, sizes[i], t1 - t0, reps[i]);
    }
    print_row("memcpy", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++) {
        /* random read from psram, write to sdram — indices in SDRAM */
        volatile uint32_t *ps = (volatile uint32_t *)psram;
        volatile uint32_t *sd = (volatile uint32_t *)sdram;
        uint32_t n_words = sizes[i] / 4;
        uint32_t *idx_tbl = (uint32_t *)src_buf;
        xor_state = 0x12345678;
        for (uint32_t k = 0; k < n_words; k++)
            idx_tbl[k] = xorshift32() % n_words;
        uint32_t t0 = of_time_us();
        for (int j = 0; j < reps[i]; j++) {
            for (uint32_t k = 0; k < n_words; k++)
                sd[k] = ps[idx_tbl[k]];
        }
        uint32_t t1 = of_time_us();
        sink = *(volatile uint8_t *)sdram;
        fmt_mbps(r[i], 16, sizes[i], t1 - t0, reps[i]);
    }
    print_row("random", r, NUM_SIZES);

    /* S->P (SDRAM to PSRAM) */
    print_header("S->P");

    for (int i = 0; i < NUM_SIZES; i++) {
        memset(sdram, 0xCC, sizes[i]);
        uint32_t t0 = of_time_us();
        for (int j = 0; j < reps[i]; j++)
            memcpy(psram, sdram, sizes[i]);
        uint32_t t1 = of_time_us();
        sink = *(volatile uint8_t *)psram;
        fmt_mbps(r[i], 16, sizes[i], t1 - t0, reps[i]);
    }
    print_row("memcpy", r, NUM_SIZES);

    for (int i = 0; i < NUM_SIZES; i++) {
        /* indices in SDRAM */
        volatile uint32_t *sd = (volatile uint32_t *)sdram;
        volatile uint32_t *ps = (volatile uint32_t *)psram;
        uint32_t n_words = sizes[i] / 4;
        uint32_t *idx_tbl = (uint32_t *)src_buf;
        xor_state = 0x12345678;
        for (uint32_t k = 0; k < n_words; k++)
            idx_tbl[k] = xorshift32() % n_words;
        uint32_t t0 = of_time_us();
        for (int j = 0; j < reps[i]; j++) {
            for (uint32_t k = 0; k < n_words; k++)
                ps[k] = sd[idx_tbl[k]];
        }
        uint32_t t1 = of_time_us();
        sink = *(volatile uint8_t *)psram;
        fmt_mbps(r[i], 16, sizes[i], t1 - t0, reps[i]);
    }
    print_row("random", r, NUM_SIZES);
}

int main(void) {
    printf("\033[2J\033[H");
    printf("Memory  Benchmark (MB/s)\n");

    run_sdram();
    run_psram();
    run_bram();
    run_cross();

    printf("\n Done. Press any button.\n");

    while (1) {
        of_input_poll();
        of_input_state_t st;
        of_input_state(0, &st);
        if (st.buttons_pressed)
            break;
        usleep(16 * 1000);
    }

    return 0;
}
