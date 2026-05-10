/*
 * of_video.h -- Video subsystem API for openfpgaOS
 *
 * 320x240 framebuffer, 8-bit indexed color, triple-buffered.
 */

#ifndef OF_VIDEO_H
#define OF_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Screen constants */
#define OF_SCREEN_W     320
#define OF_SCREEN_H     240

/* Display mode constants */
#define OF_DISPLAY_TERMINAL    0  /* Terminal only */
#define OF_DISPLAY_FRAMEBUFFER 1  /* Framebuffer only */
#define OF_DISPLAY_OVERLAY     2  /* White terminal text over framebuffer */

#ifndef OF_PC

#include "of_services.h"

static inline void of_video_init(void) {
    OF_SVC->video_init();
}

static inline uint8_t *of_video_surface(void) {
    return OF_SVC->video_get_surface();
}

static inline void of_video_flip(void) {
    OF_SVC->video_flip();
}

/* Wait for the most recent of_video_flip() to be presented (vsync). */
static inline void of_video_wait_flip(void) {
    OF_SVC->video_wait_flip();
}

static inline void of_video_clear(uint8_t color) {
    OF_SVC->video_clear(color);
}

static inline void of_video_palette(uint8_t index, uint32_t rgb) {
    OF_SVC->video_set_palette(index, rgb);
}

static inline void of_video_palette_bulk(const uint32_t *pal, int count) {
    OF_SVC->video_set_palette_bulk(pal, count);
}

/* Convert and set a VGA 6-bit palette (768 bytes: R,G,B triplets, 0-63 range).
 * Converts to 8-bit 0x00RRGGBB and sets all 256 entries at once. */
static inline void of_video_palette_vga6(const uint8_t *vga_pal, int count) {
    uint32_t pal32[256];
    for (int i = 0; i < count && i < 256; i++) {
        uint8_t r = (vga_pal[i*3+0] * 255 + 31) / 63;
        uint8_t g = (vga_pal[i*3+1] * 255 + 31) / 63;
        uint8_t b = (vga_pal[i*3+2] * 255 + 31) / 63;
        pal32[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    of_video_palette_bulk(pal32, count);
}

/* Set a VGA 4-byte palette (6-bit BGR format: B6 G6 R6 pad per entry).
 * Kernel converts 6-bit→8-bit directly — no userspace math needed. */
static inline void of_video_palette_vga4(const uint8_t *bgra6, int count) {
    OF_SVC->video_set_palette_vga4(bgra6, count);
}

static inline void of_video_flush(void) {
    OF_SVC->video_flush_cache();
}

static inline void of_video_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x < OF_SCREEN_W && (unsigned)y < OF_SCREEN_H) {
        uint8_t *fb = of_video_surface();
        fb[y * OF_SCREEN_W + x] = color;
    }
}

/* Blit a source buffer centered vertically in the 320x240 surface.
 * Clears top/bottom bars. src must be src_w * src_h bytes (8-bit indexed). */
static inline void of_video_blit_letterbox(const uint8_t *src, int src_w, int src_h) {
    uint8_t *fb = of_video_surface();
    int y_offset = (OF_SCREEN_H - src_h) / 2;
    if (y_offset > 0)
        __builtin_memset(fb, 0, OF_SCREEN_W * y_offset);
    __builtin_memcpy(fb + OF_SCREEN_W * y_offset, src, src_w * src_h);
    int bottom = y_offset + src_h;
    if (bottom < OF_SCREEN_H)
        __builtin_memset(fb + OF_SCREEN_W * bottom, 0, OF_SCREEN_W * (OF_SCREEN_H - bottom));
}

static inline void of_video_set_display_mode(int mode) {
    OF_SVC->video_set_display_mode(mode);
}

/* Color mode constants */
#define OF_VIDEO_MODE_8BIT     0  /* 8-bit indexed: 256 colors, 1 byte/pixel */
#define OF_VIDEO_MODE_4BIT     1  /* 4-bit indexed: 16 colors, 0.5 byte/pixel */
#define OF_VIDEO_MODE_2BIT     2  /* 2-bit indexed: 4 colors, 0.25 byte/pixel */
#define OF_VIDEO_MODE_RGB565   3  /* 16-bit direct: R5G6B5, 2 bytes/pixel */
#define OF_VIDEO_MODE_RGB555   4  /* 15-bit direct: X1R5G5B5, 2 bytes/pixel */
#define OF_VIDEO_MODE_RGBA5551 5  /* 15+1 bit: R5G5B5A1, 2 bytes/pixel */

/* Framebuffer size per mode (320x240) */
#define OF_FB_SIZE_8BIT     (320 * 240)         /* 76,800 bytes */
#define OF_FB_SIZE_4BIT     (320 * 240 / 2)     /* 38,400 bytes */
#define OF_FB_SIZE_2BIT     (320 * 240 / 4)     /* 19,200 bytes */
#define OF_FB_SIZE_16BPP    (320 * 240 * 2)     /* 153,600 bytes */

static inline void of_video_set_color_mode(int mode) {
    OF_SVC->video_set_color_mode(mode);
}

/* Register a callback invoked on every vsync (vblank) IRQ.
 * Pass NULL to disable. Callback runs in kernel context — keep it short. */
static inline void of_video_set_vsync_callback(void (*cb)(void)) {
    OF_SVC->video_set_vsync_callback(cb);
}

/* Get surface as 16-bit for direct color modes */
static inline uint16_t *of_video_surface16(void) {
    return (uint16_t *)of_video_surface();
}

#else /* OF_PC */

void     of_video_init(void);
uint8_t *of_video_surface(void);
void     of_video_flip(void);
void     of_video_wait_flip(void);
void     of_video_clear(uint8_t color);
void     of_video_palette(uint8_t index, uint32_t rgb);
void     of_video_palette_bulk(const uint32_t *pal, int count);
void     of_video_flush(void);
void     of_video_set_display_mode(int mode);

/* Convert and set a VGA 6-bit palette (768 bytes: R,G,B triplets, 0-63 range).
 * Converts to 8-bit 0x00RRGGBB and sets all 256 entries at once. */
static inline void of_video_palette_vga6(const uint8_t *vga_pal, int count) {
    uint32_t pal32[256];
    for (int i = 0; i < count && i < 256; i++) {
        uint8_t r = (vga_pal[i*3+0] * 255 + 31) / 63;
        uint8_t g = (vga_pal[i*3+1] * 255 + 31) / 63;
        uint8_t b = (vga_pal[i*3+2] * 255 + 31) / 63;
        pal32[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    of_video_palette_bulk(pal32, count);
}

static inline void of_video_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x < OF_SCREEN_W && (unsigned)y < OF_SCREEN_H) {
        uint8_t *fb = of_video_surface();
        fb[y * OF_SCREEN_W + x] = color;
    }
}

/* Blit a source buffer centered vertically in the 320x240 surface.
 * Clears top/bottom bars. src must be src_w * src_h bytes (8-bit indexed). */
static inline void of_video_blit_letterbox(const uint8_t *src, int src_w, int src_h) {
    uint8_t *fb = of_video_surface();
    int y_offset = (OF_SCREEN_H - src_h) / 2;
    if (y_offset > 0)
        __builtin_memset(fb, 0, OF_SCREEN_W * y_offset);
    __builtin_memcpy(fb + OF_SCREEN_W * y_offset, src, src_w * src_h);
    int bottom = y_offset + src_h;
    if (bottom < OF_SCREEN_H)
        __builtin_memset(fb + OF_SCREEN_W * bottom, 0, OF_SCREEN_W * (OF_SCREEN_H - bottom));
}

#endif /* OF_PC */

/* Blit a rectangular region from src buffer to the framebuffer.
 * Transparent: pixel value 0 is skipped. For opaque blits, use of_blit_opaque. */
static inline void of_blit(int dx, int dy, int w, int h,
                            const uint8_t *src, int src_stride) {
    uint8_t *fb = of_video_surface();
    /* Clip to screen bounds */
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > OF_SCREEN_W) w = OF_SCREEN_W - dx;
    if (dy + h > OF_SCREEN_H) h = OF_SCREEN_H - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src + (sy + y) * src_stride + sx;
        uint8_t *dp = fb + (dy + y) * OF_SCREEN_W + dx;
        for (int x = 0; x < w; x++)
            if (sp[x]) dp[x] = sp[x];
    }
}

/* Opaque blit: copies all pixels (no transparency check). Uses memcpy per row. */
static inline void of_blit_opaque(int dx, int dy, int w, int h,
                                   const uint8_t *src, int src_stride) {
    uint8_t *fb = of_video_surface();
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > OF_SCREEN_W) w = OF_SCREEN_W - dx;
    if (dy + h > OF_SCREEN_H) h = OF_SCREEN_H - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++)
        __builtin_memcpy(fb + (dy + y) * OF_SCREEN_W + dx,
                         src + (sy + y) * src_stride + sx, w);
}

/* Blit with a fixed palette offset (transparent: pixel 0 skipped). */
static inline void of_blit_pal(int dx, int dy, int w, int h,
                                const uint8_t *src, int src_stride,
                                uint8_t pal_offset) {
    uint8_t *fb = of_video_surface();
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > OF_SCREEN_W) w = OF_SCREEN_W - dx;
    if (dy + h > OF_SCREEN_H) h = OF_SCREEN_H - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src + (sy + y) * src_stride + sx;
        uint8_t *dp = fb + (dy + y) * OF_SCREEN_W + dx;
        for (int x = 0; x < w; x++)
            if (sp[x]) dp[x] = sp[x] + pal_offset;
    }
}

/* Fill a rectangle with a solid palette index. Uses memset per row. */
static inline void of_fill_rect(int x, int y, int w, int h, uint8_t color) {
    uint8_t *fb = of_video_surface();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > OF_SCREEN_W) w = OF_SCREEN_W - x;
    if (y + h > OF_SCREEN_H) h = OF_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int ry = 0; ry < h; ry++)
        __builtin_memset(fb + (y + ry) * OF_SCREEN_W + x, color, w);
}

#ifdef __cplusplus
}
#endif

#endif /* OF_VIDEO_H */
