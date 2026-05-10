/*
 * openfpgaOS Triple Buffer Demo
 * Validates triple-buffered framebuffer cycling, data retention, and clear.
 * Draws colored frames to visually demonstrate buffer rotation.
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int pass, fail;

#define ASSERT(name, cond) do { \
    if (cond) { pass++; printf("  PASS %s\n", name); } \
    else      { fail++; printf("  FAIL %s\n", name); } \
} while (0)

/* Fill framebuffer with a solid color index */
static void fill_screen(uint8_t *fb, uint8_t color) {
    memset(fb, color, OF_SCREEN_W * OF_SCREEN_H);
}

int main(void) {
    pass = 0;
    fail = 0;

    of_video_init();

    /* Set up some palette entries for visual feedback */
    of_video_palette(1, 0x00FF4444);  /* red */
    of_video_palette(2, 0x0044FF44);  /* green */
    of_video_palette(3, 0x004444FF);  /* blue */
    of_video_palette(4, 0x00FFFFFF);  /* white (pass) */
    of_video_palette(5, 0x00FF0000);  /* bright red (fail) */

    /* --- Test: surface is non-NULL --- */
    uint8_t *s0 = of_video_surface();
    ASSERT("surface", s0 != NULL);

    /* --- Test: 3 distinct buffers with vsync-paced flips --- */
    uint8_t *bufs[4];
    bufs[0] = of_video_surface();
    of_video_flip();
    of_video_wait_flip();  /* wait for vsync to consume the swap */
    bufs[1] = of_video_surface();
    of_video_flip();
    of_video_wait_flip();
    bufs[2] = of_video_surface();
    of_video_flip();
    of_video_wait_flip();
    bufs[3] = of_video_surface();

    ASSERT("cycle", bufs[3] == bufs[0]);
    ASSERT("buf 0!=1", bufs[0] != bufs[1]);
    ASSERT("buf 1!=2", bufs[1] != bufs[2]);
    ASSERT("buf 0!=2", bufs[0] != bufs[2]);

    /* --- Test: rapid flips without vsync degrade gracefully --- */
    uint8_t *r0 = of_video_surface();
    of_video_flip();
    of_video_flip();
    uint8_t *r1 = of_video_surface();
    ASSERT("rapid flip", r0 != NULL && r1 != NULL);

    /* --- Test: data retention across vsync-paced flips --- */
    bufs[0] = of_video_surface();
    bufs[0][0] = 0xAA;
    of_video_flip();
    of_video_wait_flip();
    bufs[1] = of_video_surface();
    bufs[1][0] = 0xBB;
    of_video_flip();
    of_video_wait_flip();
    bufs[2] = of_video_surface();
    bufs[2][0] = 0xCC;

    of_video_flip();
    of_video_wait_flip();
    uint8_t *back = of_video_surface();
    ASSERT("retain", back == bufs[0] && back[0] == 0xAA);

    /* --- Test: clear --- */
    of_video_clear(0x42);
    uint8_t *fb = of_video_surface();
    ASSERT("clear", fb[0] == 0x42 && fb[OF_SCREEN_W * OF_SCREEN_H - 1] == 0x42);

    /* --- Test: rapid flip stress --- */
    for (int i = 0; i < 60; i++)
        of_video_flip();
    ASSERT("flip 60x", 1);

    printf("\n  %d passed, %d failed\n", pass, fail);

    /* --- Visual demo: cycle red/green/blue frames --- */
    uint8_t colors[] = { 1, 2, 3 };
    int frame = 0;

    while (1) {
        fb = of_video_surface();
        fill_screen(fb, colors[frame % 3]);

        of_video_flip();
        usleep(500 * 1000);
        frame++;
    }
}
