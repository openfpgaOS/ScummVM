/*
 * SDL2 shim for openfpgaOS
 *
 * Minimal SDL2 implementation using of_* syscalls.
 * On PC builds, this header is never used — the real SDL2 is linked.
 *
 * Video model: the SDL screen surface points DIRECTLY at the 320x240
 * hardware framebuffer. No intermediate buffer, no copy, no cache issues.
 * SDL_Flip = of_video_flip (buffer swap).
 *
 * Covers: video (8-bit indexed surface), input, audio, timer.
 */

#ifndef _OF_SDL2_SHIM_H
#define _OF_SDL2_SHIM_H

#ifdef OF_PC
#include_next <SDL2/SDL.h>
#else

#include "of.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ======================================================================
 * Version / Constants
 * ====================================================================== */

#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL    0

#define SDL_INIT_VIDEO          0x00000020
#define SDL_INIT_AUDIO          0x00000010
#define SDL_INIT_TIMER          0x00000001
#define SDL_INIT_EVENTS         0x00004000
#define SDL_INIT_GAMECONTROLLER 0x00002000
#define SDL_INIT_EVERYTHING     0x0000FFFF

#define SDL_SWSURFACE    0x00000000
#define SDL_HWSURFACE    0x00000001
#define SDL_HWPALETTE    0x00000008
#define SDL_DOUBLEBUF    0x40000000
#define SDL_SRCCOLORKEY  0x00001000
#define SDL_PHYSPAL      1
#define SDL_LOGPAL       2

#define SDL_WINDOW_SHOWN        0x00000004
#define SDL_WINDOW_RESIZABLE    0x00000020
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_WINDOWPOS_CENTERED  0x2FFF0000
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000

#define SDL_RENDERER_ACCELERATED    0x00000002
#define SDL_RENDERER_PRESENTVSYNC   0x00000004

#define SDL_PIXELFORMAT_ARGB8888    0x16362004
#define SDL_PIXELFORMAT_RGBA32      0x16362004
#define SDL_PIXELFORMAT_RGB888      0x16161804
#define SDL_PIXELFORMAT_INDEX8      0x13000001
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET    2

#define SDL_QUIT            0x100
#define SDL_KEYDOWN         0x300
#define SDL_KEYUP           0x301
#define SDL_CONTROLLERBUTTONDOWN  0x650
#define SDL_CONTROLLERBUTTONUP    0x651
#define SDL_CONTROLLERDEVICEADDED 0x654

#define AUDIO_S16SYS    0x8010
#define AUDIO_S16       0x8010
#define AUDIO_F32SYS    0x8120
#define AUDIO_U8        0x0008

#define SDL_LIL_ENDIAN  1234
#define SDL_BIG_ENDIAN  4321
#define SDL_BYTEORDER   SDL_LIL_ENDIAN

#ifndef SDL_bool
#define SDL_bool int
#define SDL_FALSE 0
#define SDL_TRUE 1
#endif

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int16_t  Sint16;
typedef int32_t  Sint32;

#define SDL_malloc  malloc
#define SDL_free    free
#define SDL_memset  memset
#define SDL_memcpy  memcpy

/* ======================================================================
 * Types
 * ====================================================================== */

typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { uint8_t r, g, b, a; } SDL_Color;

typedef struct {
    int ncolors;
    SDL_Color colors[256];
} SDL_Palette;

typedef struct {
    uint32_t format;
    SDL_Palette *palette;
    uint8_t BitsPerPixel;
    uint8_t BytesPerPixel;
} SDL_PixelFormat;

typedef struct {
    uint32_t flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    SDL_Rect clip_rect;
    int locked;
} SDL_Surface;

typedef struct { int unused; } SDL_Window;
typedef struct { int unused; } SDL_Renderer;
typedef struct { int unused; } SDL_Texture;

typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
    SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
    SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
    SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
    SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
    SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
    SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_DELETE = 76,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230,
    SDL_NUM_SCANCODES = 512,
} SDL_Scancode;

#define SDLK_LAST SDL_NUM_SCANCODES

typedef struct {
    SDL_Scancode scancode;
    int sym;
    uint16_t mod;
} SDL_Keysym;

typedef struct {
    uint32_t type;
    struct { uint32_t type; uint8_t repeat; SDL_Keysym keysym; } key;
} SDL_Event;

typedef uint32_t SDL_AudioDeviceID;
typedef void (*SDL_AudioCallback)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint32_t size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

typedef void *SDL_mutex;

/* Game controller */
typedef enum {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX,
    SDL_CONTROLLER_BUTTON_INVALID = -1,
} SDL_GameControllerButton;

typedef enum {
    SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
} SDL_GameControllerAxis;

typedef struct { int __idx; } SDL_GameController;

/* ======================================================================
 * Internal state
 * ====================================================================== */

static SDL_Window       __sdl_win;
static SDL_Renderer     __sdl_ren;
static SDL_Texture      __sdl_tex;
static SDL_Palette      __sdl_palette;
static SDL_PixelFormat  __sdl_pixfmt;
static SDL_Surface      __sdl_surface;
static int              __sdl_inited;

static of_input_state_t __sdl_prev_input;
static of_input_state_t __sdl_curr_input;
static int              __sdl_events_pending;
static uint32_t         __sdl_pressed;
static uint32_t         __sdl_released;
static int              __sdl_event_bit;
static uint8_t          __sdl_keystate[SDL_NUM_SCANCODES];
static int              __sdl_polled;

static SDL_AudioCallback __sdl_audio_cb;
static void             *__sdl_audio_userdata;

static SDL_GameController __sdl_gc;

/* ======================================================================
 * Init / Quit
 * ====================================================================== */

static inline int SDL_Init(uint32_t flags) {
    (void)flags;
    memset(__sdl_keystate, 0, sizeof(__sdl_keystate));
    return 0;
}
static inline int SDL_InitSubSystem(uint32_t flags) { (void)flags; return 0; }
static inline void SDL_Quit(void) {}
static inline const char *SDL_GetError(void) { return ""; }

/* ======================================================================
 * Video — surface backed directly by HW framebuffer (320x240)
 *
 * Proven working sequence used by all SDK apps:
 *   of_video_clear(0) → of_video_surface() → writes → of_video_flip()
 * ====================================================================== */

static inline void __sdl_setup_surface(void) {
    if (__sdl_inited) return;
    __sdl_palette.ncolors = 256;
    memset(__sdl_palette.colors, 0, sizeof(__sdl_palette.colors));

    __sdl_pixfmt.format      = SDL_PIXELFORMAT_INDEX8;
    __sdl_pixfmt.palette     = &__sdl_palette;
    __sdl_pixfmt.BitsPerPixel  = 8;
    __sdl_pixfmt.BytesPerPixel = 1;

    __sdl_surface.format     = &__sdl_pixfmt;
    __sdl_surface.w          = OF_SCREEN_W;
    __sdl_surface.h          = OF_SCREEN_H;
    __sdl_surface.pitch      = OF_SCREEN_W;
    __sdl_surface.clip_rect  = (SDL_Rect){0, 0, OF_SCREEN_W, OF_SCREEN_H};
    __sdl_surface.locked     = 0;
    __sdl_inited = 1;
}

/* SDL2-native path */
static inline SDL_Window *SDL_CreateWindow(const char *title, int x, int y,
                                            int w, int h, uint32_t flags) {
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    of_video_init();
    __sdl_setup_surface();
    return &__sdl_win;
}
static inline void SDL_DestroyWindow(SDL_Window *w) { (void)w; }

static inline SDL_Surface *SDL_GetWindowSurface(SDL_Window *w) {
    (void)w;
    __sdl_setup_surface();
    __sdl_surface.pixels = of_video_surface();
    return &__sdl_surface;
}

static inline int SDL_UpdateWindowSurface(SDL_Window *w) {
    (void)w;
    of_video_flip();
    return 0;
}

/* SDL 1.2 compat path — returns the 320x240 HW surface directly.
 * The app renders at whatever scale it wants into this surface.
 * Requested width/height are ignored — HW is always 320x240. */
static inline SDL_Surface *SDL_SetVideoMode(int width, int height,
                                              int bpp, uint32_t flags) {
    (void)width; (void)height; (void)bpp; (void)flags;
    of_video_init();
    __sdl_setup_surface();
    of_video_clear(0);
    __sdl_surface.pixels = of_video_surface();
    return &__sdl_surface;
}

static inline SDL_Surface *SDL_GetVideoSurface(void) {
    __sdl_surface.pixels = of_video_surface();
    return &__sdl_surface;
}

/* Present the current frame and prepare the next back buffer.
 * Matches the proven SDK pattern: flip → clear → update pointer. */
static inline void SDL_Flip(SDL_Surface *s) {
    (void)s;
    of_video_flip();
    of_video_clear(0);
    __sdl_surface.pixels = of_video_surface();
}

/* ======================================================================
 * Palette
 * ====================================================================== */

static inline int SDL_SetPaletteColors(SDL_Palette *palette,
                                        const SDL_Color *colors,
                                        int first, int ncolors) {
    for (int i = 0; i < ncolors && (first + i) < 256; i++) {
        int idx = first + i;
        palette->colors[idx] = colors[i];
        uint32_t rgb = ((uint32_t)colors[i].r << 16) |
                       ((uint32_t)colors[i].g << 8) |
                       (uint32_t)colors[i].b;
        of_video_palette((uint8_t)idx, rgb);
    }
    return 0;
}

static inline void SDL_SetPalette(SDL_Surface *surf, int flag,
                                   const SDL_Color *pal, int first, int count) {
    (void)flag;
    if (!surf || !surf->format || !surf->format->palette) return;
    SDL_SetPaletteColors(surf->format->palette, pal, first, count);
}

static inline int SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *p) {
    s->format->palette = p; return 0;
}

static inline SDL_Palette *SDL_AllocPalette(int n) {
    (void)n; return &__sdl_palette;
}
static inline void SDL_FreePalette(SDL_Palette *p) { (void)p; }

/* ======================================================================
 * Drawing
 * ====================================================================== */

static inline int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect,
                                uint32_t color) {
    uint8_t c = (uint8_t)color;
    if (!dst) return -1;
    if (!rect) {
        memset(dst->pixels, c, (size_t)(dst->pitch * dst->h));
    } else {
        uint8_t *p = (uint8_t *)dst->pixels;
        int x0 = rect->x < 0 ? 0 : rect->x;
        int y0 = rect->y < 0 ? 0 : rect->y;
        int x1 = rect->x + rect->w; if (x1 > dst->w) x1 = dst->w;
        int y1 = rect->y + rect->h; if (y1 > dst->h) y1 = dst->h;
        for (int y = y0; y < y1; y++)
            memset(p + y * dst->pitch + x0, c, (size_t)(x1 - x0));
    }
    return 0;
}

static inline uint32_t SDL_MapRGB(const SDL_PixelFormat *fmt,
                                   uint8_t r, uint8_t g, uint8_t b) {
    if (fmt && fmt->palette && fmt->BitsPerPixel == 8) {
        for (int i = 0; i < fmt->palette->ncolors; i++) {
            SDL_Color c = fmt->palette->colors[i];
            if (c.r == r && c.g == g && c.b == b) return (uint32_t)i;
        }
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline int SDL_SetColorKey(SDL_Surface *s, int flag, uint32_t key) {
    (void)s; (void)flag; (void)key; return 0;
}

static inline int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }

/* ======================================================================
 * Renderer / Texture stubs
 * ====================================================================== */

static inline SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int idx,
                                                uint32_t flags) {
    (void)w; (void)idx; (void)flags; return &__sdl_ren;
}
static inline void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }
static inline int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h) {
    (void)r; (void)w; (void)h; return 0;
}
static inline SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, uint32_t fmt,
                                              int access, int w, int h) {
    (void)r; (void)fmt; (void)access; (void)w; (void)h; return &__sdl_tex;
}
static inline void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }
static inline int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r,
                                     const void *px, int pitch) {
    (void)t; (void)r; (void)px; (void)pitch; return 0;
}
static inline int SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t R,
                                          uint8_t g, uint8_t b, uint8_t a) {
    (void)r; (void)R; (void)g; (void)b; (void)a; return 0;
}
static inline int SDL_RenderClear(SDL_Renderer *r) {
    (void)r; of_video_clear(0); return 0;
}
static inline int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t,
                                  const SDL_Rect *s, const SDL_Rect *d) {
    (void)r; (void)t; (void)s; (void)d; return 0;
}
static inline void SDL_RenderPresent(SDL_Renderer *r) {
    (void)r; of_video_flip();
}

/* ======================================================================
 * Events / Input
 * ====================================================================== */

static inline void __sdl_update_keystate(void) {
    memset(__sdl_keystate, 0, sizeof(__sdl_keystate));
    uint32_t b = __sdl_curr_input.buttons;
    if (b & OF_BTN_UP)     __sdl_keystate[SDL_SCANCODE_UP] = 1;
    if (b & OF_BTN_DOWN)   __sdl_keystate[SDL_SCANCODE_DOWN] = 1;
    if (b & OF_BTN_LEFT)   __sdl_keystate[SDL_SCANCODE_LEFT] = 1;
    if (b & OF_BTN_RIGHT)  __sdl_keystate[SDL_SCANCODE_RIGHT] = 1;
    if (b & OF_BTN_A)    { __sdl_keystate[SDL_SCANCODE_Z] = 1;
                           __sdl_keystate[SDL_SCANCODE_C] = 1; }
    if (b & OF_BTN_B)    { __sdl_keystate[SDL_SCANCODE_X] = 1;
                           __sdl_keystate[SDL_SCANCODE_V] = 1; }
    if (b & OF_BTN_X)     __sdl_keystate[SDL_SCANCODE_E] = 1;
    if (b & OF_BTN_Y)     __sdl_keystate[SDL_SCANCODE_F9] = 1;
    if (b & OF_BTN_L1)    __sdl_keystate[SDL_SCANCODE_LSHIFT] = 1;
    if (b & OF_BTN_R1)    __sdl_keystate[SDL_SCANCODE_S] = 1;
    if (b & OF_BTN_L2)    __sdl_keystate[SDL_SCANCODE_D] = 1;
    if (b & OF_BTN_SELECT) __sdl_keystate[SDL_SCANCODE_F11] = 1;
    if (b & OF_BTN_START) __sdl_keystate[SDL_SCANCODE_ESCAPE] = 1;
}

static inline void __sdl_do_poll(void) {
    if (__sdl_polled) return;
    of_input_poll();
    of_input_state(0, &__sdl_curr_input);
    __sdl_pressed  = __sdl_curr_input.buttons & ~__sdl_prev_input.buttons;
    __sdl_released = ~__sdl_curr_input.buttons & __sdl_prev_input.buttons;
    __sdl_prev_input = __sdl_curr_input;
    __sdl_update_keystate();
    __sdl_polled = 1;
}

static inline SDL_Scancode __sdl_btn_to_scancode(int bit) {
    switch (1 << bit) {
    case OF_BTN_UP:     return SDL_SCANCODE_UP;
    case OF_BTN_DOWN:   return SDL_SCANCODE_DOWN;
    case OF_BTN_LEFT:   return SDL_SCANCODE_LEFT;
    case OF_BTN_RIGHT:  return SDL_SCANCODE_RIGHT;
    case OF_BTN_A:      return SDL_SCANCODE_Z;
    case OF_BTN_B:      return SDL_SCANCODE_X;
    case OF_BTN_X:      return SDL_SCANCODE_E;
    case OF_BTN_Y:      return SDL_SCANCODE_F9;
    case OF_BTN_L1:     return SDL_SCANCODE_LSHIFT;
    case OF_BTN_R1:     return SDL_SCANCODE_S;
    case OF_BTN_L2:     return SDL_SCANCODE_D;
    case OF_BTN_SELECT: return SDL_SCANCODE_F11;
    case OF_BTN_START:  return SDL_SCANCODE_ESCAPE;
    default:            return SDL_SCANCODE_UNKNOWN;
    }
}

static inline int SDL_PollEvent(SDL_Event *event) {
    if (!__sdl_events_pending) {
        __sdl_do_poll();
        __sdl_events_pending = 1;
        __sdl_event_bit = 0;
    }

    while (__sdl_event_bit < 16) {
        uint32_t mask = 1u << __sdl_event_bit;
        __sdl_event_bit++;
        if (__sdl_pressed & mask) {
            event->type = SDL_KEYDOWN;
            event->key.type = SDL_KEYDOWN;
            event->key.repeat = 0;
            event->key.keysym.scancode = __sdl_btn_to_scancode(__sdl_event_bit - 1);
            event->key.keysym.sym = event->key.keysym.scancode;
            event->key.keysym.mod = 0;
            return 1;
        }
        if (__sdl_released & mask) {
            event->type = SDL_KEYUP;
            event->key.type = SDL_KEYUP;
            event->key.repeat = 0;
            event->key.keysym.scancode = __sdl_btn_to_scancode(__sdl_event_bit - 1);
            event->key.keysym.sym = event->key.keysym.scancode;
            event->key.keysym.mod = 0;
            return 1;
        }
    }

    __sdl_events_pending = 0;
    __sdl_polled = 0;
    return 0;
}

static inline const uint8_t *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_NUM_SCANCODES;
    return __sdl_keystate;
}
#define SDL_GetKeyState SDL_GetKeyboardState

static inline int SDL_PushEvent(SDL_Event *ev) { (void)ev; return 0; }
static inline void SDL_PumpEvents(void) {}

/* ======================================================================
 * Game Controller
 * ====================================================================== */

static inline int SDL_NumJoysticks(void) { return 1; }
static inline SDL_bool SDL_IsGameController(int i) {
    return (i == 0) ? SDL_TRUE : SDL_FALSE;
}
static inline SDL_GameController *SDL_GameControllerOpen(int i) {
    (void)i; return &__sdl_gc;
}
static inline const char *SDL_GameControllerName(SDL_GameController *gc) {
    (void)gc; return "Analogue Pocket";
}
static inline void SDL_GameControllerUpdate(void) { __sdl_do_poll(); }

static inline SDL_bool SDL_GameControllerGetButton(SDL_GameController *gc,
                                                     SDL_GameControllerButton btn) {
    (void)gc;
    uint32_t b = __sdl_curr_input.buttons;
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_A:             return (b & OF_BTN_A) != 0;
    case SDL_CONTROLLER_BUTTON_B:             return (b & OF_BTN_B) != 0;
    case SDL_CONTROLLER_BUTTON_X:             return (b & OF_BTN_X) != 0;
    case SDL_CONTROLLER_BUTTON_Y:             return (b & OF_BTN_Y) != 0;
    case SDL_CONTROLLER_BUTTON_BACK:          return (b & OF_BTN_SELECT) != 0;
    case SDL_CONTROLLER_BUTTON_START:         return (b & OF_BTN_START) != 0;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return (b & OF_BTN_L1) != 0;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return (b & OF_BTN_R1) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return (b & OF_BTN_UP) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return (b & OF_BTN_DOWN) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return (b & OF_BTN_LEFT) != 0;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return (b & OF_BTN_RIGHT) != 0;
    default: return 0;
    }
}

static inline Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc,
                                                 SDL_GameControllerAxis axis) {
    (void)gc;
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:  return __sdl_curr_input.joy_lx;
    case SDL_CONTROLLER_AXIS_LEFTY:  return __sdl_curr_input.joy_ly;
    case SDL_CONTROLLER_AXIS_RIGHTX: return __sdl_curr_input.joy_rx;
    case SDL_CONTROLLER_AXIS_RIGHTY: return __sdl_curr_input.joy_ry;
    default: return 0;
    }
}

static inline SDL_GameControllerButton
SDL_GameControllerGetButtonFromString(const char *s) {
    if (!s) return SDL_CONTROLLER_BUTTON_INVALID;
    if (!strcmp(s,"a")) return SDL_CONTROLLER_BUTTON_A;
    if (!strcmp(s,"b")) return SDL_CONTROLLER_BUTTON_B;
    if (!strcmp(s,"start")) return SDL_CONTROLLER_BUTTON_START;
    if (!strcmp(s,"leftshoulder")) return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    if (!strcmp(s,"rightshoulder")) return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    if (!strcmp(s,"dpup")) return SDL_CONTROLLER_BUTTON_DPAD_UP;
    if (!strcmp(s,"dpdown")) return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    if (!strcmp(s,"dpleft")) return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    if (!strcmp(s,"dpright")) return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    return SDL_CONTROLLER_BUTTON_INVALID;
}

static inline const char *
SDL_GameControllerGetStringForButton(SDL_GameControllerButton b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "a";
    case SDL_CONTROLLER_BUTTON_B: return "b";
    case SDL_CONTROLLER_BUTTON_START: return "start";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "leftshoulder";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "rightshoulder";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "dpup";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "dpdown";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "dpleft";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "dpright";
    default: return "?";
    }
}

/* ======================================================================
 * Timer
 * ====================================================================== */

static inline uint32_t SDL_GetTicks(void) { return of_time_ms(); }
static inline void SDL_Delay(uint32_t ms) { of_delay_ms(ms); }

/* ======================================================================
 * Audio
 * ====================================================================== */

static inline SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device,
        int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained,
        int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    of_audio_init();
    if (desired->callback) {
        __sdl_audio_cb = desired->callback;
        __sdl_audio_userdata = desired->userdata;
    }
    if (obtained) *obtained = *desired;
    return 1;
}

static inline void SDL_CloseAudioDevice(SDL_AudioDeviceID d) {
    (void)d; __sdl_audio_cb = 0;
}
static inline void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int p) {
    (void)d; (void)p;
}

static inline void SDL_AudioPump(void) {
    if (__sdl_audio_cb) {
        int free_pairs = of_audio_free();
        if (free_pairs > 256) free_pairs = 256;
        if (free_pairs > 0) {
            int16_t buf[512];
            __sdl_audio_cb(__sdl_audio_userdata, (uint8_t *)buf, free_pairs * 4);
            of_audio_write(buf, free_pairs);
        }
    }
}

static inline int SDL_QueueAudio(SDL_AudioDeviceID d, const void *data, uint32_t len) {
    (void)d;
    of_audio_write((const int16_t *)data, (int)(len / 4));
    return 0;
}

static inline SDL_AudioSpec *SDL_LoadWAV_RW(void *src, int freesrc,
                                             SDL_AudioSpec *spec,
                                             uint8_t **audio_buf,
                                             uint32_t *audio_len) {
    (void)src; (void)freesrc; (void)spec;
    *audio_buf = 0; *audio_len = 0; return 0;
}

static inline SDL_AudioSpec *SDL_LoadWAV(const char *file, SDL_AudioSpec *spec,
                                          uint8_t **audio_buf, uint32_t *audio_len) {
    *audio_buf = 0; *audio_len = 0;
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0 || size > 4*1024*1024) { fclose(f); return 0; }
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, (size_t)size, f);
    fclose(f);
    of_codec_result_t result;
    if (of_codec_parse_wav(data, (uint32_t)size, &result) < 0) { free(data); return 0; }
    spec->freq = (int)result.sample_rate;
    spec->format = (result.bits_per_sample == 16) ? AUDIO_S16SYS : AUDIO_U8;
    spec->channels = result.channels;
    spec->silence = 0; spec->samples = 4096;
    spec->size = result.pcm_len; spec->callback = 0; spec->userdata = 0;
    uint8_t *pcm = (uint8_t *)malloc(result.pcm_len);
    if (!pcm) { free(data); return 0; }
    memcpy(pcm, result.pcm, result.pcm_len);
    free(data);
    *audio_buf = pcm; *audio_len = result.pcm_len;
    return spec;
}

static inline void SDL_FreeWAV(uint8_t *buf) { free(buf); }

static inline void SDL_MixAudioFormat(uint8_t *dst, const uint8_t *src,
                                       uint16_t fmt, uint32_t len, int vol) {
    (void)fmt;
    const int16_t *s = (const int16_t *)src;
    int16_t *d = (int16_t *)dst;
    for (uint32_t i = 0; i < len/2; i++) {
        int32_t m = (int32_t)d[i] + (((int32_t)s[i] * vol) >> 7);
        if (m > 32767) m = 32767; if (m < -32768) m = -32768;
        d[i] = (int16_t)m;
    }
}
#define SDL_MIX_MAXVOLUME 128

/* ======================================================================
 * Misc stubs
 * ====================================================================== */

static inline SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)1; }
static inline void SDL_DestroyMutex(SDL_mutex *m) { (void)m; }
static inline int SDL_LockMutex(SDL_mutex *m) { (void)m; return 0; }
static inline int SDL_UnlockMutex(SDL_mutex *m) { (void)m; return 0; }

static inline void SDL_WM_SetCaption(const char *t, const char *i) { (void)t; (void)i; }
static inline int SDL_WM_ToggleFullScreen(SDL_Surface *s) { (void)s; return 1; }
static inline int SDL_SetWindowFullscreen(SDL_Window *w, uint32_t f) { (void)w;(void)f; return 0; }
static inline void SDL_SetWindowTitle(SDL_Window *w, const char *t) { (void)w;(void)t; }

/* ======================================================================
 * SDL2 compat defines — map SDLK_* to SDL_SCANCODE_*
 * ====================================================================== */

#define SDLK_F9      SDL_SCANCODE_F9
#define SDLK_ESCAPE  SDL_SCANCODE_ESCAPE
#define SDLK_DELETE  SDL_SCANCODE_DELETE
#define SDLK_F11     SDL_SCANCODE_F11
#define SDLK_LSHIFT  SDL_SCANCODE_LSHIFT
#define SDLK_LEFT    SDL_SCANCODE_LEFT
#define SDLK_RIGHT   SDL_SCANCODE_RIGHT
#define SDLK_UP      SDL_SCANCODE_UP
#define SDLK_DOWN    SDL_SCANCODE_DOWN
#define SDLK_5       SDL_SCANCODE_5
#define SDLK_a       SDL_SCANCODE_A
#define SDLK_b       SDL_SCANCODE_B
#define SDLK_c       SDL_SCANCODE_C
#define SDLK_d       SDL_SCANCODE_D
#define SDLK_e       SDL_SCANCODE_E
#define SDLK_m       SDL_SCANCODE_M
#define SDLK_n       SDL_SCANCODE_N
#define SDLK_s       SDL_SCANCODE_S
#define SDLK_v       SDL_SCANCODE_V
#define SDLK_x       SDL_SCANCODE_X
#define SDLK_z       SDL_SCANCODE_Z

#define sym scancode

#endif /* OF_PC */
#endif /* _OF_SDL2_SHIM_H */
