/*
 * openfpgaOS BRAM Demo
 *
 * Benchmarks a tight loop running from BRAM vs SDRAM.
 * Demonstrates the OF_FASTTEXT API for placing hot code in BRAM.
 */

#include "of.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================
 * Test function: 16.16 fixed-point multiply (tight inner loop)
 *
 * Two identical implementations — one in BRAM, one in SDRAM.
 * The BRAM version should be faster due to zero I-cache miss latency.
 * ====================================================================== */

typedef int32_t fixed_t;
#define FRACBITS 16

/* BRAM version — zero wait state */
OF_FASTTEXT static fixed_t
fixed_mul_bram(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}

/* SDRAM version — subject to I-cache misses */
static fixed_t __attribute__((noinline))
fixed_mul_sdram(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}

/* Prevent the compiler from optimizing away the loop */
static volatile fixed_t sink;

#define ITERATIONS 1000000

static uint32_t bench(fixed_t (*fn)(fixed_t, fixed_t)) {
    fixed_t a = 0x00018000;  /* 1.5 */
    fixed_t b = 0x00028000;  /* 2.5 */

    uint32_t t0 = of_time_us();
    for (int i = 0; i < ITERATIONS; i++) {
        a = fn(a, b);
        b = fn(b, a);
    }
    uint32_t t1 = of_time_us();
    sink = a + b;
    return t1 - t0;
}

int main(void) {
    printf("\033[2J\033[H");
    printf("  openfpgaOS BRAM Benchmark\n");
    printf("  =========================\n\n");

    printf("  OF_FASTTEXT function at: 0x%08X (BRAM)\n", (unsigned)(uintptr_t)fixed_mul_bram);
    printf("  SDRAM function at:       0x%08X (SDRAM)\n", (unsigned)(uintptr_t)fixed_mul_sdram);
    printf("  Iterations: %d\n\n", ITERATIONS);

    printf("  Running SDRAM benchmark...\n");
    uint32_t sdram_us = bench(fixed_mul_sdram);

    printf("  Running BRAM benchmark...\n");
    uint32_t bram_us = bench(fixed_mul_bram);

    printf("\n");
    printf("  Results:\n");
    printf("    SDRAM: %u us\n", sdram_us);
    printf("    BRAM:  %u us\n", bram_us);

    if (bram_us > 0) {
        uint32_t speedup_x10 = (sdram_us * 10) / bram_us;
        printf("    Speedup: %u.%ux\n", speedup_x10 / 10, speedup_x10 % 10);
    }

    printf("\n  Done. Press any button.\n");

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
