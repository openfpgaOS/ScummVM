/*
 * freadalign — minimal reproducer for the fread-alignment bug.
 *
 * Background:
 *   fread() on an SDK-opened FILE* silently returns 0 bytes when the
 *   destination pointer is not 512-byte aligned. Aligned destinations
 *   read correctly. See docs/bug-fread-alignment.md for the full
 *   context and proposed fix.
 *
 * What this app does:
 *   1. Opens the first real file on the card (via opendir/readdir).
 *   2. Reads the first 16 bytes into a 512-byte-aligned buffer.
 *   3. Reads the first 16 bytes into a stack-local buffer (usually
 *      4- or 8-byte aligned, never 512-aligned).
 *   4. Prints both return values and the first 16 bytes of each dest.
 *
 * Expected POSIX-conformant behavior:
 *   aligned   -> got=16 buf=<first 16 bytes of file>
 *   unaligned -> got=16 buf=<first 16 bytes of file>   (same data)
 *
 * Observed on current kernel (2026-04-22):
 *   aligned   -> got=16 buf=<first 16 bytes of file>
 *   unaligned -> got=0  buf=00 00 00 00 ...            (DMA silently dropped)
 *
 * A passing run of this test is the acceptance criterion for the
 * kernel-side fix. Build with the standard SDK app toolchain, copy to
 * the Pocket, run.
 */

#include "of.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>

/* 512-byte aligned destination (known-good). */
static uint8_t aligned_buf[512] __attribute__((aligned(512)));

static void hexdump16(const uint8_t *b, int got) {
    for (int i = 0; i < 16 && i < got; i++) printf("%02x ", b[i]);
    for (int i = got; i < 16; i++)          printf(".. ");
}

static const char *pick_file(char *out, size_t outsz) {
    DIR *d = opendir("/");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        /* Skip the app's own ELF and any file too small to probe. */
        if (stat(e->d_name, &st) != 0) continue;
        if (st.st_size < 512) continue;
        snprintf(out, outsz, "%s", e->d_name);
        closedir(d);
        return out;
    }
    closedir(d);
    return NULL;
}

int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\033[2J\033[H");
    printf("freadalign — fread destination-alignment bug reproducer\n\n");

    char fname[64];
    if (!pick_file(fname, sizeof(fname))) {
        printf("No suitable file found on card.\n");
        for (;;) usleep(1000000);
    }
    printf("Using file: %s\n\n", fname);

    /* ---------- Test 1: aligned destination ---------- */
    FILE *f = fopen(fname, "rb");
    if (!f) {
        printf("fopen('%s') failed\n", fname);
        for (;;) usleep(1000000);
    }
    memset(aligned_buf, 0, sizeof(aligned_buf));
    int g1 = (int)fread(aligned_buf, 1, 16, f);
    fclose(f);

    printf("aligned   (dst=%p): got=%d buf=", (void *)aligned_buf, g1);
    hexdump16(aligned_buf, g1);
    printf("\n");

    /* ---------- Test 2: unaligned (stack) destination ---------- */
    uint8_t stack_buf[16];   /* stack — 4 or 8 byte aligned, not 512. */
    memset(stack_buf, 0, sizeof(stack_buf));

    f = fopen(fname, "rb");
    if (!f) {
        printf("fopen('%s') failed (second open)\n", fname);
        for (;;) usleep(1000000);
    }
    int g2 = (int)fread(stack_buf, 1, 16, f);
    fclose(f);

    printf("unaligned (dst=%p): got=%d buf=", (void *)stack_buf, g2);
    hexdump16(stack_buf, g2);
    printf("\n\n");

    /* ---------- Verdict ---------- */
    if (g1 == g2 && g1 == 16 && memcmp(aligned_buf, stack_buf, 16) == 0) {
        printf("PASS — reads match. Kernel handles unaligned fread correctly.\n");
    } else if (g1 == 16 && g2 == 0) {
        printf("FAIL — unaligned fread returned 0 bytes (classic alignment bug).\n");
        printf("       See docs/bug-fread-alignment.md for the fix.\n");
    } else {
        printf("FAIL — unexpected mismatch between aligned and unaligned reads.\n");
    }

    for (;;) usleep(1000000);
    return 0;
}
