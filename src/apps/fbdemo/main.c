/*
 * openfpgaOS Framebuffer Demo
 * Loads an indexed PNG image and displays it.
 */

#include <stdio.h>
#include <string.h>

#include "of.h"
#include <time.h>
#include <unistd.h>
#include "png.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define MAX_PNG_SIZE (128 * 1024)

static uint8_t png_buf[MAX_PNG_SIZE];
static uint8_t pixel_buf[SCREEN_W * SCREEN_H + SCREEN_H + MAX_PNG_SIZE];
static uint32_t raw_palette[256];

int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_OVERLAY);

    printf("Loading image...\n");

    FILE *f = fopen("slot:3", "rb");
    if (!f) {
        printf("Error: cannot open slot:3\n");
        while (1) { usleep(100 * 1000); }
    }
    uint32_t file_size = (uint32_t)fread(png_buf, 1, MAX_PNG_SIZE, f);
    fclose(f);

    if (file_size == 0) {
        printf("Error: empty file\n");
        while (1) { usleep(100 * 1000); }
    }
    printf("PNG size: %d bytes\n", (int)file_size);

    int w, h;
    int rc = png_decode(png_buf, file_size, raw_palette, pixel_buf, &w, &h);
    if (rc < 0) {
        printf("PNG decode error: %d\n", rc);
        while (1) { usleep(100 * 1000); }
    }
    printf("Decoded %dx%d\n", w, h);

    /* Set palette */
    of_video_palette_bulk(raw_palette, 256);

    /* Clear and blit image centered */
    of_video_clear(0);
    uint8_t *fb = of_video_surface();

    int ox = (SCREEN_W - w) / 2;
    int oy = (SCREEN_H - h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    int copy_w = w < SCREEN_W ? w : SCREEN_W;
    int copy_h = h < SCREEN_H ? h : SCREEN_H;
    for (int y = 0; y < copy_h; y++)
        memcpy(&fb[(oy + y) * SCREEN_W + ox], &pixel_buf[y * w], copy_w);

    of_video_flip();

    /* Clear terminal and show overlay text on top of the image */
    printf("\033[2J\033[H");
    printf("\033[%d;%dH", 0+1, 0+1);
    printf("OVERLAY MODE - Press any button");

    /* Cycle through display modes on each button press:
     * 0=Terminal, 1=Framebuffer, 2=Overlay */
    static const char *mode_names[] = {
        "Terminal", "Framebuffer", "Overlay"
    };
    int mode = OF_DISPLAY_OVERLAY;  /* start in overlay */

    /* Wait for menu button to be released */
    for (int i = 0; i < 30; i++) {
        of_input_poll();
        usleep(16 * 1000);
    }

    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A) || of_btn_pressed(OF_BTN_B) ||
            of_btn_pressed(OF_BTN_START)) {
            mode = (mode + 1) % 3;
            of_video_set_display_mode(mode);
            printf("\033[2J\033[H");
            printf("\033[%d;%dH", 0+1, 0+1);
            printf("Mode: ");
            printf("%s", mode_names[mode]);
        }
        usleep(16 * 1000);
    }

    return 0;
}
