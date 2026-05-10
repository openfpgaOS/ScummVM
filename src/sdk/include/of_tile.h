/*
 * of_tile.h -- Tile layer & sprite engine API for openfpgaOS
 *
 * 64x32 tilemap of 8x8 tiles, 4bpp with 16 sub-palettes.
 * 64 hardware sprites, 8x8 pixels, 4bpp.
 */

#ifndef OF_TILE_H
#define OF_TILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_TILE_COLS    64
#define OF_TILE_ROWS    32
#define OF_TILE_SIZE    8
#define OF_MAX_TILES    256
#define OF_MAX_SPRITES  64

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Tile Layer */

static inline void of_tile_enable(int enable, int priority) {
    of_ecall2(OF_EID_TILE, OF_TILE_FID_ENABLE, enable, priority);
}

static inline void of_tile_scroll(int x, int y) {
    of_ecall2(OF_EID_TILE, OF_TILE_FID_SCROLL, x, y);
}

static inline void of_tile_set(int col, int row, uint16_t entry) {
    of_ecall3(OF_EID_TILE, OF_TILE_FID_SET, col, row, entry);
}

static inline void of_tile_load_map(const uint16_t *data,
                                     int x, int y, int w, int h) {
    of_ecall5(OF_EID_TILE, OF_TILE_FID_LOAD_MAP, (long)data, x, y, w, h);
}

static inline void of_tile_load_chr(int first_tile,
                                     const void *data, int num_tiles) {
    of_ecall3(OF_EID_TILE, OF_TILE_FID_LOAD_CHR,
              first_tile, (long)data, num_tiles);
}

/* Sprite Engine */

static inline void of_sprite_enable(int enable) {
    of_ecall1(OF_EID_SPRITE, OF_SPRITE_FID_ENABLE, enable);
}

static inline void of_sprite_set(int index, int x, int y,
                                  int tile_id, int palette,
                                  int hflip, int vflip, int enable) {
    long packed = (palette & 0xF) |
                  ((hflip & 1) << 4) |
                  ((vflip & 1) << 5) |
                  ((enable & 1) << 6);
    of_ecall5(OF_EID_SPRITE, OF_SPRITE_FID_SET, index, x, y, tile_id, packed);
}

static inline void of_sprite_move(int index, int x, int y) {
    of_ecall3(OF_EID_SPRITE, OF_SPRITE_FID_MOVE, index, x, y);
}

static inline void of_sprite_load_chr(int first_tile,
                                       const void *data, int num_tiles) {
    of_ecall3(OF_EID_SPRITE, OF_SPRITE_FID_LOAD_CHR,
              first_tile, (long)data, num_tiles);
}

static inline void of_sprite_hide(int index) {
    of_ecall1(OF_EID_SPRITE, OF_SPRITE_FID_HIDE, index);
}

static inline void of_sprite_hide_all(void) {
    of_ecall0(OF_EID_SPRITE, OF_SPRITE_FID_HIDE_ALL);
}

#else /* OF_PC */

/* Tile Layer */
void of_tile_enable(int enable, int priority);
void of_tile_scroll(int x, int y);
void of_tile_set(int col, int row, uint16_t entry);
void of_tile_load_map(const uint16_t *data, int x, int y, int w, int h);
void of_tile_load_chr(int first_tile, const void *data, int num_tiles);

/* Sprite Engine */
void of_sprite_enable(int enable);
void of_sprite_set(int index, int x, int y, int tile_id, int palette,
                   int hflip, int vflip, int enable);
void of_sprite_move(int index, int x, int y);
void of_sprite_load_chr(int first_tile, const void *data, int num_tiles);
void of_sprite_hide(int index);
void of_sprite_hide_all(void);

#endif /* OF_PC */

/* Tile entry helper (pure computation, available on both platforms) */
static inline uint16_t of_tile_entry(int tile_id, int palette,
                                      int hflip, int vflip) {
    return (uint16_t)(
        (tile_id & 0xFF) |
        ((palette & 0xF) << 10) |
        (hflip ? (1 << 14) : 0) |
        (vflip ? (1 << 15) : 0)
    );
}

#ifdef __cplusplus
}
#endif

#endif /* OF_TILE_H */
