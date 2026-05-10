#include "test.h"
#include "of_mixer.h"
#include <time.h>

/* PSRAM / SRAM memory map.
 *
 * CRAM0 is strictly i_axi-only on the data plane (see
 * GenOpenFpgaVexii.scala — CRAM0 is excluded from the d_axi PMA so
 * the D$ can never reach it; that's the prerequisite for sync-burst
 * CRAM0 refills to not corrupt under multi-master contention).
 * Apps that need to touch CRAM0 as data MUST use the uncached alias
 * 0x38xxxxxx, which routes through p_axi (uncached IO bus).  A
 * cached 0x30xxxxxx store will trap with "store access fault".
 *
 *   CRAM0 cached:    0x30000000 – 0x30FFFFFF (16 MB) — I-fetch only
 *   CRAM0 uncached:  0x38000000 – 0x38FFFFFF (16 MB) — d/IO access
 *   CRAM1 uncached:  0x39000000 – 0x39FFFFFF (16 MB, bridge-accessible)
 *   SRAM:            0x3A000000 – 0x3A03FFFF (256 KB)
 *
 * CRAM1 is FULLY partitioned by the OS:
 *   0x000000 – 0x27FFFF  saves (10 slots × 256 KB) — bridge-persisted
 *   0x280000 – 0x28FFFF  FTAB (filename table) — bridge-written
 *   0x290000 – 0x2FFFFF  file-IO scratch (~448 KB)
 *   0x300000 – 0x3FFFFF  I/O cache
 *   0x400000 – 0xEFFFFF  audio samples (11 MB)
 *   0xF00000 – 0xFFFFFF  AUDIO_SCRATCH_CRAM1 (1 MB) — mixer scratch
 *
 * Picking ANY region the bridge / mixer / file layer touches is
 * non-idempotent across iterations: the original test wrote slot 0
 * (saves) which the bridge then re-read on the next pass; the unused
 * tail turned out to be the audio scratch buffer.
 *
 * file-IO scratch is the only region that is (a) not bridge-managed
 * and (b) only touched during user-initiated file ops. test_psram_memory
 * runs BEFORE any file test, so its readback can't race the OS — and
 * between iterations the test's own pattern is the last write to land. */
#define CRAM0_CACHED_BASE   0x30000000  /* I-fetch only */
#define CRAM0_UNCACHED_BASE 0x38000000  /* d-side access */
#define CRAM0_TEST_OFFSET   0x00800000  /* 8 MB offset — avoids slot table at base */
#define CRAM1_UNCACHED_BASE 0x39000000  /* CRAM1 uncached (bridge-accessible) */
#define CRAM1_TEST_OFFSET   0x00290000  /* file-IO scratch — see comment above */
#define SRAM_BASE           0x3A000000  /* On-chip SRAM */

/* SDRAM region used to evict D-cache lines (must not overlap app code/data) */
#define SDRAM_EVICT_BASE    0x13F00000

void test_psram_memory(void) {
    section_start("PSRAM Mem");

    /* v2 memory arch retires CRAM1 + SRAM as CPU-addressable memory.
     * CRAM0 is kept but its only role is bridge staging (load/save
     * scratch), time-sliced with the CPU via an MMIO owner bit.
     * The old uncached aliases 0x38xxxxxx / 0x39xxxxxx / 0x3Axxxxxx
     * are no longer in any PMA region — a direct load/store traps.
     *
     * Skip the whole PSRAM memory test suite here: none of the
     * hardware it was probing exists as an app-addressable region
     * anymore.  The per-chip memory controllers are already exercised
     * by the standalone Verilator harnesses in src/fpga/test/. */
    test_pass("skipped (v2)");
    section_end();
    return;
#if 0
    /* Any pre-v2 target: original test below kept for reference. */
    if (of_get_caps()->platform_id != OF_PLATFORM_POCKET) {
        test_pass("not pocket");
        section_end();
        return;
    }

    /* DIAGNOSTIC: stop any lingering mixer voices to isolate whether the
     * CRAM1 hang on iteration 2+ is caused by mixer state bleed-through.
     * If this makes the hang go away, mixer is the culprit. */
    of_mixer_stop_all();

    /* Use the uncached alias for all CRAM0 data-plane traffic — the
     * cached alias (0x30xxxxxx) is i_axi-only under the current PMA. */
    volatile uint32_t *cram0 = (volatile uint32_t *)(CRAM0_UNCACHED_BASE + CRAM0_TEST_OFFSET);
    volatile uint32_t *cram1 = (volatile uint32_t *)(CRAM1_UNCACHED_BASE + CRAM1_TEST_OFFSET);
    // SRAM removed — GPU-exclusive (Z-buffer)

    cram0[0] = 0xDEADBEEF;
    cram0[1] = 0xCAFEBABE;
    ASSERT("cram0 w/r[0]", cram0[0] == 0xDEADBEEF);
    ASSERT("cram0 w/r[1]", cram0[1] == 0xCAFEBABE);

    {
        int ok = 1;
        for (int i = 0; i < 32; i++)
            cram0[i] = 1u << i;
        for (int i = 0; i < 32; i++) {
            if (cram0[i] != (1u << i)) { ok = 0; break; }
        }
        ASSERT("cram0 walk1", ok);
    }

    {
        int ok = 1;
        for (uint32_t i = 0; i < 16384; i++)
            cram0[i] = i ^ 0x5A5A5A5A;
        for (uint32_t i = 0; i < 16384; i++) {
            if (cram0[i] != (i ^ 0x5A5A5A5A)) { ok = 0; break; }
        }
        ASSERT("cram0 64K", ok);
    }

    {
        for (uint32_t i = 0; i < 16384; i++)
            cram0[i] = i;

        uint32_t t0 = of_time_us();
        volatile uint32_t sum = 0;
        for (uint32_t i = 0; i < 16384; i++)
            sum += cram0[i];
        uint32_t t1 = of_time_us();
        (void)sum;

        uint32_t elapsed_us = t1 - t0;
        ASSERT("cram0 speed", elapsed_us < 100000);
        (void)elapsed_us;
    }

    cram1[0] = 0x12345678;
    cram1[1] = 0x9ABCDEF0;
    ASSERT("cram1 w/r[0]", cram1[0] == 0x12345678);
    ASSERT("cram1 w/r[1]", cram1[1] == 0x9ABCDEF0);

    {
        /* Snapshot the test region so the test is fully idempotent.
         * No CRAM1 region is truly idle across iterations: saves are
         * bridge-persisted, file-IO scratch and I/O cache are touched
         * by file ops, samples are touched by the mixer, the audio
         * scratch tail is touched by the streamer. Whatever was here
         * before us is restored on the way out, and the OS sees no
         * disturbance from one iteration to the next.
         *
         * 16 KB lives in BSS (SDRAM), not on the BRAM stack. */
        static uint32_t cram1_save[4096];
        for (uint32_t i = 0; i < 4096; i++)
            cram1_save[i] = cram1[i];

        int ok = 1;
        uint32_t fail_at = 0, fail_exp = 0, fail_got = 0;
        for (uint32_t i = 0; i < 4096; i++)
            cram1[i] = ~i;
        for (uint32_t i = 0; i < 4096; i++) {
            uint32_t got = cram1[i];
            uint32_t exp = ~i;
            if (got != exp) {
                ok = 0;
                fail_at = i;
                fail_exp = exp;
                fail_got = got;
                break;
            }
        }

        /* Restore original contents BEFORE asserting so the failure
         * detail printf doesn't allocate / call into anything that
         * might re-touch CRAM1 with the test pattern still in place. */
        for (uint32_t i = 0; i < 4096; i++)
            cram1[i] = cram1_save[i];

        if (ok) {
            test_pass("cram1 16K");
        } else {
            snprintf(__buf, sizeof(__buf),
                     "@%lu e%08lx g%08lx",
                     (unsigned long)fail_at,
                     (unsigned long)fail_exp,
                     (unsigned long)fail_got);
            test_fail("cram1 16K", __buf);
        }
    }

    // SRAM tests removed — SRAM is GPU-exclusive (Z-buffer)

    {
        cram1[0] = 0x11111111;
        cram0[0] = 0x22222222;
        ASSERT("isolation c1", cram1[0] == 0x11111111);
        ASSERT("isolation c0", cram0[0] == 0x22222222);
    }

    {
        volatile uint8_t *cram0_b = (volatile uint8_t *)(CRAM0_UNCACHED_BASE + CRAM0_TEST_OFFSET);
        cram0[0] = 0x00000000;
        cram0_b[1] = 0xAB;
        ASSERT("byte write", (cram0[0] & 0x0000FF00) == 0x0000AB00);
    }

    {
        int ok = 1;
        for (int i = 0; i < 256; i++) {
            cram0[i] = (uint32_t)i;
            cram1[i] = (uint32_t)(i + 0x1000);
        }
        for (int i = 0; i < 256; i++) {
            if (cram0[i] != (uint32_t)i) { ok = 0; break; }
            if (cram1[i] != (uint32_t)(i + 0x1000)) { ok = 0; break; }
        }
        ASSERT("interleave", ok);
    }

    section_end();
#endif /* pre-v2 reference code */
}

void test_cram0_256k(void) {
    section_start("CRAM0 256K");

    /* v2: skipped.  See note in test_psram_memory(). */
    test_pass("skipped (v2)");
    section_end();
    return;
#if 0
    if (of_get_caps()->platform_id != OF_PLATFORM_POCKET) {
        test_pass("not pocket");
        section_end();
        return;
    }

    /* CRAM0 is i_axi-only under the current PMA — all data-plane
     * access uses the 0x38xxxxxx uncached alias, which routes through
     * p_axi.  The cached→uncached write-then-read pattern used to
     * verify D-cache writeback is no longer meaningful on this target
     * (cached writes trap with store-access-fault). */

    /* Uncached 32-bit write then read */
    {
        volatile uint32_t *u = (volatile uint32_t *)(CRAM0_UNCACHED_BASE + CRAM0_TEST_OFFSET);
        u[0] = 0xDEAD1234;
        __asm__ volatile("fence" ::: "memory");
        uint32_t v = u[0];
        ASSERT("uc w/r", v == 0xDEAD1234);
    }

    volatile uint32_t *cram0_u = (volatile uint32_t *)(CRAM0_UNCACHED_BASE + CRAM0_TEST_OFFSET);
    const uint32_t count = 256 * 1024 / 4;  /* 65536 words = 256KB */

    /* XOR pattern: write + read back via uncached alias */
    for (uint32_t i = 0; i < count; i++)
        cram0_u[i] = i ^ 0xA5A5A5A5;
    __asm__ volatile("fence" ::: "memory");

    {
        int ok = 1;
        uint32_t fail_addr = 0, fail_exp = 0, fail_got = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t exp = i ^ 0xA5A5A5A5;
            uint32_t got = cram0_u[i];
            if (got != exp) {
                ok = 0;
                fail_addr = i;
                fail_exp = exp;
                fail_got = got;
                break;
            }
        }
        if (ok) {
            test_pass("xor pattern");
        } else {
            snprintf(__buf, sizeof(__buf),
                     "@%lu exp %08lx go %08lx",
                     (unsigned long)fail_addr,
                     (unsigned long)fail_exp,
                     (unsigned long)fail_got);
            test_fail("xor pattern", __buf);
        }
    }

    /* Inverted index pattern */
    for (uint32_t i = 0; i < count; i++)
        cram0_u[i] = ~i;
    __asm__ volatile("fence" ::: "memory");

    {
        int ok = 1;
        uint32_t fail_addr = 0, fail_exp = 0, fail_got = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t exp = ~i;
            uint32_t got = cram0_u[i];
            if (got != exp) {
                ok = 0;
                fail_addr = i;
                fail_exp = exp;
                fail_got = got;
                break;
            }
        }
        if (ok) {
            test_pass("inv pattern");
        } else {
            snprintf(__buf, sizeof(__buf),
                     "@%lu exp %08lx go %08lx",
                     (unsigned long)fail_addr,
                     (unsigned long)fail_exp,
                     (unsigned long)fail_got);
            test_fail("inv pattern", __buf);
        }
    }

    section_end();
#endif /* pre-v2 reference code */
}
