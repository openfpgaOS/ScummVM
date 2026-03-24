/*
 * Celeste Classic -- openfpgaOS port (SDL shim)
 *
 * Adapted from lemon32767/ccleste sdl12main.c.
 * Uses the openfpgaOS SDL2 shim — surface points directly at the
 * 320x240 HW framebuffer. Renders at 2x scale with centering.
 *
 * Game logic: celeste.c/celeste.h (MIT license, lemon32767)
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "celeste.h"
#include "tilemap.h"

/* ======================================================================
 * Globals
 * ====================================================================== */

static SDL_Surface *screen = NULL;

#define PICO8_W 128
#define PICO8_H 128

/* Scale=2: render 128x128 at 2x into 320x240, centered with clipping */
static const int scale = 2;
#define OFS_X ((320 - PICO8_W * 2) / 2)   /* 32 */
#define OFS_Y ((240 - PICO8_H * 2) / 2)   /* -8 */

/* ======================================================================
 * Palette — PICO-8 16 colors, stored as SDL_Color for getcolor()
 * ====================================================================== */

static const SDL_Color base_palette[16] = {
    {0x00,0x00,0x00,0xFF}, {0x1D,0x2B,0x53,0xFF},
    {0x7E,0x25,0x53,0xFF}, {0x00,0x87,0x51,0xFF},
    {0xAB,0x52,0x36,0xFF}, {0x5F,0x57,0x4F,0xFF},
    {0xC2,0xC3,0xC7,0xFF}, {0xFF,0xF1,0xE8,0xFF},
    {0xFF,0x00,0x4D,0xFF}, {0xFF,0xA3,0x00,0xFF},
    {0xFF,0xEC,0x27,0xFF}, {0x00,0xE4,0x36,0xFF},
    {0x29,0xAD,0xFF,0xFF}, {0x83,0x76,0x9C,0xFF},
    {0xFF,0x77,0xA8,0xFF}, {0xFF,0xCC,0xAA,0xFF},
};
static SDL_Color palette[16];

static inline Uint8 getcolor(int idx) {
    SDL_Color c = palette[idx & 15];
    for (int i = 0; i < 16; i++)
        if (base_palette[i].r == c.r && base_palette[i].g == c.g && base_palette[i].b == c.b)
            return (Uint8)i;
    return (Uint8)(idx & 15);
}

static void ResetPalette(void) {
    memcpy(palette, base_palette, sizeof palette);
}

/* ======================================================================
 * Sprite / font data (embedded BMP)
 * ====================================================================== */

static uint8_t gfx_data[128 * 64];
static uint8_t font_data[128 * 85];

static int load_bmp_data(const uint8_t *bmp, int bmp_len, uint8_t *out,
                         int w, int h, int bpp) {
    if (bmp_len < 54) return -1;
    int data_offset = bmp[10]|(bmp[11]<<8)|(bmp[12]<<16)|(bmp[13]<<24);
    int bmp_w = bmp[18]|(bmp[19]<<8)|(bmp[20]<<16)|(bmp[21]<<24);
    int bmp_h = bmp[22]|(bmp[23]<<8)|(bmp[24]<<16)|(bmp[25]<<24);
    int top_down = 0;
    if (bmp_h < 0) { bmp_h = -bmp_h; top_down = 1; }
    if (bmp_w != w || bmp_h != h) return -2;
    int row_bytes = (bpp == 4) ? (w+1)/2 : (w+7)/8;
    int row_stride = (row_bytes + 3) & ~3;
    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h-1-y);
        const uint8_t *row = bmp + data_offset + src_y * row_stride;
        for (int x = 0; x < w; x++) {
            uint8_t pixel;
            if (bpp == 4) { uint8_t b = row[x/2]; pixel = (x&1) ? (b&0x0F) : (b>>4); }
            else { pixel = (row[x/8] >> (7-(x&7))) & 1; }
            out[y*w+x] = pixel;
        }
    }
    return 0;
}

#include "gfx_data.h"

static void LoadData(void) {
    printf("loading gfx...");
    load_bmp_data(gfx_bmp, gfx_bmp_len, gfx_data, 128, 64, 4);
    printf("done\nloading font...");
    load_bmp_data(font_bmp, font_bmp_len, font_data, 128, 85, 1);
    printf("done\n");
}

/* ======================================================================
 * Drawing — render at 2x scale with centering, directly to HW FB
 *
 * All functions take PICO-8 coordinates (0..127) and apply SCALE + OFS.
 * screen->pixels points directly at the 320x240 hardware framebuffer.
 * ====================================================================== */

static inline void Xblit(int sprite, int x, int y, int flipx, int flipy) {
    int sx = (sprite % 16) * 8;
    int sy = (sprite / 16) * 8;
    uint8_t *fb = (uint8_t *)screen->pixels;

    for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
            int gx = sx + (flipx ? (7-px) : px);
            int gy = sy + (flipy ? (7-py) : py);
            if (gy >= 64) continue;
            uint8_t pixel = gfx_data[gy * 128 + gx];
            if (pixel == 0) continue;
            uint8_t c = getcolor(pixel);
            /* Write scale×scale block */
            for (int ssy = 0; ssy < scale; ssy++) {
                int fy = OFS_Y + (y+py) * scale + ssy;
                if ((unsigned)fy >= 240) continue;
                for (int ssx = 0; ssx < scale; ssx++) {
                    int fx = OFS_X + (x+px) * scale + ssx;
                    if ((unsigned)fx >= 320) continue;
                    fb[fy * 320 + fx] = c;
                }
            }
        }
    }
}

static void p8_rectfill(int x0, int y0, int x1, int y1, int col) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    Uint8 c = getcolor(col);
    SDL_Rect rc = { OFS_X + x0*scale, OFS_Y + y0*scale,
                    (x1-x0+1)*scale, (y1-y0+1)*scale };
    SDL_FillRect(screen, &rc, c);
}

static void p8_line(int x0, int y0, int x1, int y1, int col) {
    Uint8 c = getcolor(col);
    uint8_t *fb = (uint8_t *)screen->pixels;
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx-dy;
    for (;;) {
        for (int ssy = 0; ssy < scale; ssy++) {
            int fy = OFS_Y + y0*scale + ssy;
            if ((unsigned)fy >= 240) continue;
            for (int ssx = 0; ssx < scale; ssx++) {
                int fx = OFS_X + x0*scale + ssx;
                if ((unsigned)fx >= 320) continue;
                fb[fy*320+fx] = c;
            }
        }
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void p8_print(const char *str, int x, int y, int col) {
    Uint8 c = getcolor(col);
    uint8_t *fb = (uint8_t *)screen->pixels;
    for (const char *s = str; *s; s++) {
        int ch = (*s) & 0x7F;
        int fx = (ch%16)*8, fy = (ch/16)*8;
        for (int py = 0; py < 8; py++)
            for (int px = 0; px < 4; px++)
                if (fy+py < 85 && font_data[(fy+py)*128+fx+px])
                    for (int ssy = 0; ssy < scale; ssy++) {
                        int dy = OFS_Y + (y+py)*scale + ssy;
                        if ((unsigned)dy >= 240) continue;
                        for (int ssx = 0; ssx < scale; ssx++) {
                            int dx = OFS_X + (x+px)*scale + ssx;
                            if ((unsigned)dx >= 320) continue;
                            fb[dy*320+dx] = c;
                        }
                    }
        x += 4;
    }
}

static void p8_circfill(int cx, int cy, int r, int col) {
    if (r <= 0) { p8_rectfill(cx,cy,cx,cy,col); return; }
    /* Horizontal-line fill for each scanline */
    Uint8 c = getcolor(col);
    int xx = r, yy = 0, err = 1-r;
    while (xx >= yy) {
        p8_rectfill(cx-xx, cy+yy, cx+xx, cy+yy, col);
        p8_rectfill(cx-xx, cy-yy, cx+xx, cy-yy, col);
        p8_rectfill(cx-yy, cy+xx, cx+yy, cy+xx, col);
        p8_rectfill(cx-yy, cy-xx, cx+yy, cy-xx, col);
        (void)c;
        yy++;
        if (err < 0) err += 2*yy+1;
        else { xx--; err += 2*(yy-xx)+1; }
    }
}

static void p8_map(int mx, int my, int tx, int ty, int mw, int mh, int mask,
                   int cam_x, int cam_y) {
    for (int y = 0; y < mh; y++)
        for (int x = 0; x < mw; x++) {
            int tile = tilemap_data[(x+mx)+(y+my)*128];
            int draw = 0;
            if (mask == 0) draw = 1;
            else if (mask == 4 && tile_flags[tile] == 4) draw = 1;
            else if (mask != 4 && tile < (int)(sizeof(tile_flags)/sizeof(*tile_flags))
                     && (tile_flags[tile] & (1<<(mask-1)))) draw = 1;
            if (draw)
                Xblit(tile, tx+x*8-cam_x, ty+y*8-cam_y, 0, 0);
        }
}

/* ======================================================================
 * OSD
 * ====================================================================== */

static char osd_text[200] = "";
static int osd_timer = 0;
static void OSDset(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(osd_text, sizeof osd_text, fmt, ap);
    osd_timer = 30; va_end(ap);
}
static void OSDdraw(void) {
    if (osd_timer > 0) {
        --osd_timer;
        int x = 4, y = 120 + (osd_timer<10 ? 10-osd_timer : 0);
        int len = (int)strlen(osd_text);
        p8_rectfill(x-2,y-2, x+4*len,y+6, 6);
        p8_rectfill(x-1,y-1, x+4*len-1,y+5, 0);
        p8_print(osd_text, x, y, 7);
    }
}

/* ======================================================================
 * PICO-8 callback
 * ====================================================================== */

static int camera_x, camera_y;
static int enable_screenshake = 1;
static Uint16 buttons_state;

static int pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...) {
    va_list args; int ret = 0;
    va_start(args, call);
    #define INT_ARG()  va_arg(args, int)
    #define BOOL_ARG() (Celeste_P8_bool_t)va_arg(args, int)

    switch (call) {
    case CELESTE_P8_MUSIC: { INT_ARG(); INT_ARG(); INT_ARG(); } break;
    case CELESTE_P8_SPR: {
        int spr=INT_ARG(), x=INT_ARG(), y=INT_ARG();
        int cols=INT_ARG(), rows=INT_ARG();
        int fx=BOOL_ARG(), fy=BOOL_ARG();
        (void)cols;(void)rows;
        if (spr>=0) Xblit(spr, x-camera_x, y-camera_y, fx, fy);
    } break;
    case CELESTE_P8_BTN: { int b=INT_ARG(); ret=(buttons_state&(1<<b))!=0; } break;
    case CELESTE_P8_SFX: { INT_ARG(); } break;
    case CELESTE_P8_PAL: {
        int a=INT_ARG(), b=INT_ARG();
        if (a>=0&&a<16&&b>=0&&b<16) palette[a]=base_palette[b];
    } break;
    case CELESTE_P8_PAL_RESET: { ResetPalette(); } break;
    case CELESTE_P8_CIRCFILL: {
        int cx=INT_ARG()-camera_x, cy=INT_ARG()-camera_y;
        int r=INT_ARG(), col=INT_ARG();
        p8_circfill(cx,cy,r,col);
    } break;
    case CELESTE_P8_PRINT: {
        const char *str=va_arg(args,const char*);
        int x=INT_ARG()-camera_x, y=INT_ARG()-camera_y, col=INT_ARG()%16;
        p8_print(str,x,y,col);
    } break;
    case CELESTE_P8_RECTFILL: {
        int x0=INT_ARG()-camera_x, y0=INT_ARG()-camera_y;
        int x1=INT_ARG()-camera_x, y1=INT_ARG()-camera_y;
        int col=INT_ARG();
        p8_rectfill(x0,y0,x1,y1,col);
    } break;
    case CELESTE_P8_LINE: {
        int x0=INT_ARG()-camera_x, y0=INT_ARG()-camera_y;
        int x1=INT_ARG()-camera_x, y1=INT_ARG()-camera_y;
        int col=INT_ARG();
        p8_line(x0,y0,x1,y1,col);
    } break;
    case CELESTE_P8_MGET: {
        int tx=INT_ARG(), ty=INT_ARG();
        ret=tilemap_data[tx+ty*128];
    } break;
    case CELESTE_P8_CAMERA: {
        if (enable_screenshake) { camera_x=INT_ARG(); camera_y=INT_ARG(); }
    } break;
    case CELESTE_P8_FGET: {
        int tile=INT_ARG(), flag=INT_ARG();
        ret=tile<(int)(sizeof(tile_flags)/sizeof(*tile_flags))
            &&(tile_flags[tile]&(1<<flag))!=0;
    } break;
    case CELESTE_P8_MAP: {
        int mx=INT_ARG(),my=INT_ARG(),tx=INT_ARG(),ty=INT_ARG();
        int mw=INT_ARG(),mh=INT_ARG(),mask=INT_ARG();
        p8_map(mx,my,tx,ty,mw,mh,mask,camera_x,camera_y);
    } break;
    }
    va_end(args);
    return ret;
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    /* SDL_SetVideoMode: inits video, clears buffer, returns HW surface */
    screen = SDL_SetVideoMode(320, 240, 8, SDL_SWSURFACE);

    /* Set PICO-8 palette on hardware */
    SDL_SetPalette(screen, SDL_PHYSPAL|SDL_LOGPAL,
                   (SDL_Color *)base_palette, 0, 16);
    ResetPalette();

    Mix_OpenAudio(22050, AUDIO_S16SYS, 1, 1024);

    printf("now loading...\n");
    LoadData();

    Celeste_P8_set_call_func(pico8emu);

    void *initial_state = SDL_malloc(Celeste_P8_get_state_size());
    if (initial_state) Celeste_P8_save_state(initial_state);
    Celeste_P8_set_rndseed(SDL_GetTicks());
    Celeste_P8_init();
    printf("ready\n");

    int running = 1;
    while (running) {
        /* Input: keyboard state is updated during SDL_PollEvent */
        const Uint8 *kbstate = SDL_GetKeyState(NULL);

        /* Hold Y (F9) to reset */
        static int reset_timer = 0;
        if (initial_state && kbstate[SDLK_F9]) {
            if (++reset_timer >= 30) {
                reset_timer = 0;
                OSDset("reset");
                Celeste_P8_load_state(initial_state);
                Celeste_P8_set_rndseed(SDL_GetTicks());
                Celeste_P8_init();
            }
        } else reset_timer = 0;

        buttons_state = 0;
        SDL_GameControllerUpdate();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
        }

        if (kbstate[SDLK_LEFT])  buttons_state |= (1<<0);
        if (kbstate[SDLK_RIGHT]) buttons_state |= (1<<1);
        if (kbstate[SDLK_UP])    buttons_state |= (1<<2);
        if (kbstate[SDLK_DOWN])  buttons_state |= (1<<3);
        if (kbstate[SDLK_z]||kbstate[SDLK_c]) buttons_state |= (1<<4);
        if (kbstate[SDLK_x]||kbstate[SDLK_v]) buttons_state |= (1<<5);

        Celeste_P8_update();
        Celeste_P8_draw();
        OSDdraw();

        /* SDL_Flip: flip → clear next buffer → update surface pointer */
        SDL_Flip(screen);

        /* 30 fps */
        static unsigned frame_start = 0;
        unsigned frame_end = SDL_GetTicks();
        unsigned frame_time = frame_end - frame_start;
        if (frame_time < 33) SDL_Delay(33 - frame_time);
        frame_start = SDL_GetTicks();
    }

    if (initial_state) SDL_free(initial_state);
    Mix_CloseAudio();
    SDL_Quit();
    return 0;
}
