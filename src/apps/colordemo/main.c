/*
 * openfpgaOS Color Mode Demo
 *
 * Cycles through all video color modes:
 *   8-bit indexed (256 colors)
 *   4-bit indexed (16 colors)
 *   2-bit indexed (4 colors)
 *   RGB565 (16-bit direct)
 *   RGB555 (15-bit direct)
 *   RGBA5551 (15-bit + alpha)
 *
 * Press A to advance, B to go back.
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define W 320
#define H 240

static const char *mode_names[] = {
    "8-bit indexed (256 colors)",
    "4-bit indexed (16 colors)",
    "2-bit indexed (4 colors)",
    "RGB565 (16-bit direct)",
    "RGB555 (15-bit direct)",
    "RGBA5551 (15+1 bit)",
};

/* Draw test pattern for 8-bit indexed mode */
static void draw_8bit(void) {
    uint8_t *fb = of_video_surface();

    /* Set a rainbow palette */
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        if (i < 43)       { r = 255;             g = i * 6;          b = 0; }
        else if (i < 86)  { r = 255 - (i-43)*6;  g = 255;            b = 0; }
        else if (i < 128) { r = 0;               g = 255;            b = (i-86)*6; }
        else if (i < 171) { r = 0;               g = 255-(i-128)*6;  b = 255; }
        else if (i < 214) { r = (i-171)*6;       g = 0;              b = 255; }
        else              { r = 255;             g = 0;              b = 255-(i-214)*6; }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
        of_video_palette(i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }

    /* Gradient + color bars */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (y < 120)
                fb[y * W + x] = (uint8_t)((x + y) & 0xFF);
            else
                fb[y * W + x] = (uint8_t)(x * 256 / W);
        }
    }
}

/* Draw test pattern for 4-bit indexed mode */
static void draw_4bit(void) {
    uint8_t *fb = of_video_surface();

    /* Set 16-color palette (CGA-style) */
    static const uint32_t cga[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    for (int i = 0; i < 16; i++)
        of_video_palette(i, cga[i]);

    /* 4-bit: 2 pixels per byte, low nibble first */
    int stride = W / 2;  /* 160 bytes per line */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x += 2) {
            uint8_t c0 = (uint8_t)((x / (W/16)) & 0xF);
            uint8_t c1 = (uint8_t)(((x+1) / (W/16)) & 0xF);
            fb[y * stride + x/2] = (c1 << 4) | c0;
        }
    }
}

/* Draw test pattern for 2-bit indexed mode */
static void draw_2bit(void) {
    uint8_t *fb = of_video_surface();

    /* Game Boy palette */
    of_video_palette(0, 0xE0F8D0);  /* lightest */
    of_video_palette(1, 0x88C070);
    of_video_palette(2, 0x346856);
    of_video_palette(3, 0x081820);  /* darkest */

    /* 2-bit: 4 pixels per byte */
    int stride = W / 4;  /* 80 bytes per line */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x += 4) {
            uint8_t c0 = (uint8_t)((x / (W/4)) & 0x3);
            uint8_t c1 = (uint8_t)(((x+1) / (W/4)) & 0x3);
            uint8_t c2 = (uint8_t)(((x+2) / (W/4)) & 0x3);
            uint8_t c3 = (uint8_t)(((x+3) / (W/4)) & 0x3);
            fb[y * stride + x/4] = c0 | (c1 << 2) | (c2 << 4) | (c3 << 6);
        }
    }
}

/* Draw test pattern for RGB565 mode */
static void draw_rgb565(void) {
    uint16_t *fb = of_video_surface16();

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t r = (uint16_t)(x * 31 / W);
            uint16_t g = (uint16_t)(y * 63 / H);
            uint16_t b = (uint16_t)((W - 1 - x) * 31 / W);
            fb[y * W + x] = (r << 11) | (g << 5) | b;
        }
    }
}

/* Draw test pattern for RGB555 mode */
static void draw_rgb555(void) {
    uint16_t *fb = of_video_surface16();

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t r = (uint16_t)(x * 31 / W);
            uint16_t g = (uint16_t)(y * 31 / H);
            uint16_t b = (uint16_t)((W - 1 - x) * 31 / W);
            fb[y * W + x] = (r << 10) | (g << 5) | b;
        }
    }
}

/* Draw test pattern for RGBA5551 mode */
static void draw_rgba5551(void) {
    uint16_t *fb = of_video_surface16();

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t r = (uint16_t)(x * 31 / W);
            uint16_t g = (uint16_t)(y * 31 / H);
            uint16_t b = (uint16_t)((W - 1 - x) * 31 / W);
            /* Checkerboard alpha: every other 8x8 block is transparent */
            uint16_t a = ((x / 8 + y / 8) & 1) ? 1 : 0;
            fb[y * W + x] = (r << 11) | (g << 6) | (b << 1) | a;
        }
    }
}

typedef void (*draw_fn_t)(void);
static const draw_fn_t draw_fns[] = {
    draw_8bit, draw_4bit, draw_2bit,
    draw_rgb565, draw_rgb555, draw_rgba5551,
};
#define NUM_MODES ((int)(sizeof(draw_fns) / sizeof(draw_fns[0])))

int main(void) {
    of_video_init();
    int mode = 0;

    while (1) {
        /* Set color mode */
        of_video_set_color_mode(mode);

        /* Clear framebuffer */
        uint8_t *fb = of_video_surface();
        for (int i = 0; i < W * H * 2; i++) fb[i] = 0;

        /* Draw test pattern */
        draw_fns[mode]();

        /* Show mode name at top */
        printf("\033[2J\033[H  Color Mode: %s\n", mode_names[mode]);
        printf("  A=next  B=prev\n");

        of_video_flip();

        /* Wait for button press */
        while (1) {
            of_input_poll();
            if (of_btn_pressed(OF_BTN_A)) {
                mode = (mode + 1) % NUM_MODES;
                break;
            }
            if (of_btn_pressed(OF_BTN_B)) {
                mode = (mode + NUM_MODES - 1) % NUM_MODES;
                break;
            }
            usleep(16 * 1000);
        }
        usleep(200 * 1000);  /* debounce */
    }

    return 0;
}
