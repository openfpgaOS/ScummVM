/*
 * openfpga_osystem.cpp -- ScummVM OSystem backend for openfpgaOS
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_osystem.h"

#include "backends/mutex/null/null-mutex.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "common/config-manager.h"
#include "common/memstream.h"
#include "common/stream.h"
#include "openfpga_audiocd.h"
#include "openfpga_fs.h"
#include "openfpga_midi.h"
#include "openfpga_mixer.h"
#include "openfpga_save.h"
#include "splash_logo.h"
#include "audio/mixer.h"
#include "graphics/font.h"
#include "graphics/fontman.h"
#include "graphics/surface.h"

extern "C" {
#include <of_cache.h>
#include <of_file.h>
#include <of_gpu.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
}

/* ═══════════════════════════════════════════════════════════════════
 * OpenFPGAGraphicsManager
 * ═══════════════════════════════════════════════════════════════════ */

/* Self-pointer so the free function openfpga_show_splash() can reach the
 * graphics manager without a cross-cast through the virtual BaseBackend. */
static OpenFPGAGraphicsManager *g_splashGfx = nullptr;

/* Audio-pump re-entrancy flag.  Set while ANY audio pump runs so a nested FS
 * read pulled by the mixer/CDDA -- which funnels through openfpga_pump_during_
 * load() -> openfpga_drive_audio_and_timers() on the FS read path -- no-ops
 * instead of re-entering the pump mid-mix.  CRITICAL: delayMillis()'s per-ms
 * pumps must set it too (via pumpAudioTick).  They used NOT to (they called
 * openfpga_audiocd_pump() / mgr->update() directly), so a CDDA/speech ISO read
 * re-entered the mixer mid-block and corrupted the output -- the LucasArts
 * CD-audio dropout and talkie-speech stutter.  SCI music is MIDI (no FS read
 * while playing), so it was spared, which made it look SCUMM-specific.  Declared
 * here (not just before delayMillis) so serviceCursorFromWork() can also bail
 * when a pump is in flight -- no cursor GPU flip mid audio-mix. */
static bool g_pumpBusy = false;

OpenFPGAGraphicsManager::OpenFPGAGraphicsManager()
    : _screenW(320), _screenH(200), _screenChangeID(0),
      _overlayVisible(false), _paletteDirty(false), _screenDirty(false),
      _cursorX(160), _cursorY(100), _cursorHotX(0), _cursorHotY(0),
      _cursorW(0), _cursorH(0), _cursorKeycolor(0), _cursorVisible(false),
      _gpuReady(false), _videoBufIdx(-1), _videoFence(0), _gpuCleanMask(0),
      _gpuStalled(false), _gpuStallToken(0),
      _splashActive(false), _keypadMode(false),
      _lastEnginePresentMs(0) {
    memset(_screenBuf, 0, sizeof(_screenBuf));
    memset(_palette, 0, sizeof(_palette));
    memset(_cursorData, 0, sizeof(_cursorData));
    g_splashGfx = this;
}

OpenFPGAGraphicsManager::~OpenFPGAGraphicsManager() {
}

static void getFramebufferMode(uint &fbW, uint &fbH, uint &fbStride) {
    of_video_mode_t mode;
    of_video_get_mode(&mode);

    if (mode.width == 0 || mode.height == 0 ||
        mode.color_mode != OF_VIDEO_MODE_8BIT) {
        /* Fall back to the physical 320x240 panel default, NOT the
         * OPENFPGA_SCREEN_W/H max (now 640x480): if this branch is ever hit
         * while a small game is live, a 640x480 guess would mis-size the blit. */
        fbW = 320;
        fbH = 240;
        fbStride = 320;
        return;
    }

    fbW = mode.width;
    fbH = mode.height;
    fbStride = mode.stride ? mode.stride : mode.width;
}

static uint32_t getFramebufferBytes() {
    uint fbW, fbH, fbStride;
    getFramebufferMode(fbW, fbH, fbStride);
    return fbStride * fbH;
}

/* Program the source framebuffer to the smallest hardware preset that fits the
 * game's surface.  This MUST be size-driven, not fixed to OPENFPGA_SCREEN_W/H:
 * the macros are now a 640x480 MAX, but programming 640x480 for EVERY game would
 * downscale the <=320x240 titles (SCUMM/AGI/SCI0-1.1) to a quarter of the panel.
 * Small games stay on the 320x240 full-screen scaler slot; only the larger
 * SCI32 surfaces switch to 640x480 (scaler slot 7).  Always 8-bit CLUT8. */
static void configureFramebufferMode(uint gameW, uint gameH) {
    const uint wantW = (gameW > 320 || gameH > 240) ? 640 : 320;
    const uint wantH = (gameW > 320 || gameH > 240) ? 480 : 240;

    /* Idempotent: skip the set_mode if the framebuffer is already in the target
     * mode.  This runs on every game's first frame (the splash->game handoff in
     * updateScreen), and re-latching the SAME mode would add a needless flash to
     * the existing <=320x240 8-bit games, which never had one before. */
    of_video_mode_t cur;
    of_video_get_mode(&cur);
    if (cur.width == wantW && cur.height == wantH &&
        cur.color_mode == OF_VIDEO_MODE_8BIT)
        return;

    of_video_mode_t mode;
    memset(&mode, 0, sizeof(mode));
    mode.width = wantW;
    mode.height = wantH;
    mode.color_mode = OF_VIDEO_MODE_8BIT;

    if (of_video_set_mode(&mode) != 0) {
        /* No 640x480 support on this firmware: the FB stays at its prior size
         * and a larger surface gets cropped to the top-left.  Log it so a
         * cropped SCI32 game is diagnosable over UART, not a silent mystery. */
        warning("[gfx] of_video_set_mode(%ux%u 8bit) failed; framebuffer stays "
                "at prior size, a larger surface will be cropped", wantW, wantH);
        of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
    }
}

static void fbReset();   /* dirty-rect present state; defined below near copyRectToScreen */

void OpenFPGAGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
    if (width > OPENFPGA_SCREEN_W)
        width = OPENFPGA_SCREEN_W;
    if (height > OPENFPGA_SCREEN_H)
        height = OPENFPGA_SCREEN_H;
    _screenW = width;
    _screenH = height;
    memset(_screenBuf, 0, sizeof(_screenBuf));
    fbReset();   /* surface size changed: every video buffer is now stale */
    _screenChangeID++;
    _screenDirty = true;

    /* Game graphics only -- main() will flip to FRAMEBUFFER right
     * before engine->run().  Configure the color mode and clear here.
     *
     * EXCEPT while the boot splash is up: the engine calls initSize() during
     * init -- long before it has a real frame -- and an unconditional
     * clear+flip blanked the logo to black for the whole (slow) data load.
     * The mode is already configured for the splash (same fixed 8-bit FB), so
     * skip the blank entirely; the first real updateScreen() replaces the
     * logo with game content (and clears _splashActive). */
    if (!_splashActive) {
        configureFramebufferMode(_screenW, _screenH);
        of_video_clear(0);
        of_video_flip();
    }
}

void OpenFPGAGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
    if (start >= 256)
        return;
    if (start + num > 256)
        num = 256 - start;
    memcpy(_palette + start * 3, colors, num * 3);
    _paletteDirty = true;
}

void OpenFPGAGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
    if (start >= 256)
        return;
    if (start + num > 256)
        num = 256 - start;
    memcpy(colors, _palette + start * 3, num * 3);
}

/* ===== dirty-rect (bounding-box) present optimization ======================
 * SCI/engines redraw only what changed and hand us exact rects via
 * copyRectToScreen(), but updateScreen() used to blit AND cache-flush the full
 * 640x480 every frame.  In a point-and-click like LSL7 a typical frame changes
 * only a talking head / walking actor, so copying+flushing the union bounding
 * box of recent changes instead of the whole surface cuts the per-frame work
 * ~5-10x.  Video is triple-buffered (indices 0..2, strict round-robin), so a
 * buffer is reused every 3rd frame and must receive every change since IT was
 * last drawn -- we union the per-frame dirty boxes back to that buffer's last
 * render (tracked per index) and fall back to a full blit whenever the gap
 * exceeds our short history, a frame dirtied the whole surface, the keypad
 * overlay is up, or the surface was reconfigured.  Over-blitting is always
 * correct; only UNDER-blitting corrupts, so every _screenBuf writer marks dirty
 * and any uncertainty forces a full blit. */
static const int kFbHist  = 6;   /* dirty-box ring depth (>= the 3 video buffers) */
static const int kFbSlots = 4;   /* video buffer indices are 0..2 */
static Common::Rect s_fbBox[kFbHist];     /* union dirty box for each recent frame */
static bool         s_fbFull[kFbHist];    /* that frame dirtied the whole surface */
static uint32       s_fbFrame = 0;        /* monotonic frame counter */
static int32        s_fbDrawn[kFbSlots];  /* frame# each video buffer last received (-1 = stale) */
static Common::Rect s_fbCursor[kFbSlots]; /* cursor box last drawn into each video buffer */
static bool         s_fbReady = false;

static void fbReset() {            /* force a full blit into every buffer */
    for (int i = 0; i < kFbSlots; ++i) { s_fbDrawn[i] = -1; s_fbCursor[i] = Common::Rect(); }
    for (int i = 0; i < kFbHist; ++i)  { s_fbBox[i] = Common::Rect(); s_fbFull[i] = false; }
    s_fbReady = true;
}
static inline void fbMarkBox(const Common::Rect &r) {
    if (!s_fbReady) fbReset();
    if (r.isEmpty()) return;
    int s = (int)(s_fbFrame % kFbHist);
    if (s_fbBox[s].isEmpty()) s_fbBox[s] = r; else s_fbBox[s].extend(r);
}
static inline void fbMarkFull() {
    if (!s_fbReady) fbReset();
    s_fbFull[(int)(s_fbFrame % kFbHist)] = true;
}

void OpenFPGAGraphicsManager::copyRectToScreen(const void *buf, int pitch,
                                                int x, int y, int w, int h) {
    const byte *src = (const byte *)buf;

    /* Clip */
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= y * pitch; h += y; y = 0; }
    if (x + w > (int)_screenW) w = _screenW - x;
    if (y + h > (int)_screenH) h = _screenH - y;
    if (w <= 0 || h <= 0) return;

    byte *dst = _screenBuf + y * _screenW + x;
    if (x == 0 && (uint)w == _screenW && pitch == (int)_screenW) {
        /* Full-width, source rows contiguous at our stride: one memcpy for the
         * whole span (room loads / palMorph / transitions / full-frame VMD go
         * from _screenH memcpy calls to one). */
        memcpy(dst, src, (size_t)h * _screenW);
    } else {
        for (int row = 0; row < h; row++) {
            memcpy(dst, src, w);
            dst += _screenW;
            src += pitch;
        }
    }
    fbMarkBox(Common::Rect(x, y, x + w, y + h));
    _screenDirty = true;
}

Graphics::Surface *OpenFPGAGraphicsManager::lockScreen() {
    _frameSurface.init(_screenW, _screenH, _screenW,
                       _screenBuf, Graphics::PixelFormat::createFormatCLUT8());
    return &_frameSurface;
}

void OpenFPGAGraphicsManager::unlockScreen() {
    fbMarkFull();   /* engine wrote the locked surface directly -- extent unknown */
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::fillScreen(uint32 col) {
    memset(_screenBuf, col & 0xFF, _screenW * _screenH);
    fbMarkFull();
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::fillScreen(const Common::Rect &r, uint32 col) {
    int top = r.top < 0 ? 0 : r.top;
    int left = r.left < 0 ? 0 : r.left;
    int bottom = r.bottom > (int)_screenH ? (int)_screenH : r.bottom;
    int right = r.right > (int)_screenW ? (int)_screenW : r.right;
    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            _screenBuf[y * _screenW + x] = col & 0xFF;
        }
    }
    fbMarkBox(Common::Rect(left, top, right, bottom));
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::ensureGpuReady() {
    if (_gpuReady)
        return;

    of_gpu_init();
    _videoBufIdx = of_video_acquire_next(-1, 0);
    _videoFence = 0;
    _gpuCleanMask = 0;
    _gpuReady = true;
}

uint8_t *OpenFPGAGraphicsManager::acquireFrameBuffer() {
    ensureGpuReady();
    if (_videoBufIdx < 0)
        return of_video_surface();

    uint8_t *fb = of_video_buffer_addr(_videoBufIdx);
    if (!(_gpuCleanMask & (1u << _videoBufIdx))) {
        of_cache_flush_range(fb, getFramebufferBytes());
        _gpuCleanMask |= (1u << _videoBufIdx);
    }
    return fb;
}

bool OpenFPGAGraphicsManager::waitGpuFenceBounded(uint32 token) {
    /* of_gpu_wait() spins 50M iterations (~5 s) and then __builtin_trap()s,
     * which crashes the whole machine whenever the platform menu freezes the
     * display.  A healthy border clear retires in microseconds, so a much
     * smaller budget never trips on a real frame yet lets us bail long before
     * the trap.  ~8M ≈ 0.8 s at 100 MHz with a ~10-cycle body: comfortably
     * above any legitimate frame, comfortably below the SDK's hang threshold.
     * The one-time stall it costs on menu-open is hidden behind the menu. */
    uint32_t spins = 8000000u;
    while (!of_gpu_fence_reached(token)) {
        if (--spins == 0)
            return false;
    }
    return true;
}

void OpenFPGAGraphicsManager::clearFrameBorders(uint8_t *fb, uint fbW,
                                                uint fbH, uint fbStride,
                                                int xOff, int yOff,
                                                uint copyW, uint copyH) {
    /* If we latched a stall (menu open), re-probe the pending fence with a
     * plain register read -- no command emission, so it can never hang on the
     * unbounded ring-space spin.  Once it retires the menu has closed and the
     * GPU is live again; fall through to the normal path and resume. */
    if (_gpuStalled && fb)
        _gpuStalled = !of_gpu_fence_reached(_gpuStallToken);

    /* CPU fallback: GPU never came up, or it is stalled and we must not emit
     * any commands.  Scrub the whole buffer; updateScreen() overwrites the
     * centre with the game frame immediately after this returns. */
    if (!_gpuReady || _gpuStalled || !fb) {
        if (fb)
            memset(fb, 0, fbStride * fbH);
        return;
    }

    const uint32_t fbAddr = (uint32_t)(uintptr_t)fb;
    bool issued = false;
    of_gpu_set_framebuffer(fbAddr, fbStride);

    if (yOff > 0) {
        of_gpu_clear_rect(fbAddr, fbW, yOff, 0);
        issued = true;
    }

    const int bottomY = yOff + (int)copyH;
    if (bottomY < (int)fbH) {
        of_gpu_clear_rect(fbAddr + (uint32_t)bottomY * fbStride,
                          fbW, fbH - bottomY, 0);
        issued = true;
    }

    if (copyH != 0 && xOff > 0) {
        of_gpu_clear_rect(fbAddr + (uint32_t)yOff * fbStride,
                          xOff, copyH, 0);
        issued = true;
    }

    const int rightX = xOff + (int)copyW;
    if (copyH != 0 && rightX < (int)fbW) {
        of_gpu_clear_rect(fbAddr + (uint32_t)yOff * fbStride + rightX,
                          fbW - rightX, copyH, 0);
        issued = true;
    }

    if (issued) {
        const uint32_t token = of_gpu_submit();
        if (!waitGpuFenceBounded(token)) {
            /* GPU went dark mid-frame (menu opened while we were emitting).
             * Latch the stall, stop here, and scrub the borders on the CPU so
             * the frame is not left half-cleared.  presentFrame() will skip
             * the flip; we resume once `token` finally retires. */
            _gpuStalled = true;
            _gpuStallToken = token;
            memset(fb, 0, fbStride * fbH);
        }
    }
}

void OpenFPGAGraphicsManager::presentFrame() {
    /* While the GPU is stalled (menu open) emit nothing: a flip command would
     * pile into a ring the frozen GPU never drains, and acquire_next() would
     * block on a fence that never retires.  The menu owns the screen anyway;
     * the first live frame after recovery presents normally. */
    if (_gpuStalled)
        return;

    if (_gpuReady && _videoBufIdx >= 0) {
        _videoFence = of_gpu_flip_to(_videoBufIdx);
        of_gpu_kick();
        _videoBufIdx = of_video_acquire_next(_videoBufIdx, _videoFence);
    } else {
        of_video_flush();
        of_video_flip();
    }
}

void OpenFPGAGraphicsManager::updateScreen() {
    /* Stamp the engine-present time so presentCursor() knows the game is
     * actively redrawing and can skip its own (redundant, GPU-contending) flip. */
    _lastEnginePresentMs = of_time_ms();
    presentFrameInternal();
}

void OpenFPGAGraphicsManager::presentFrameInternal() {
    /* The engine is presenting a real frame now -- let initSize() blank
     * normally again (this frame replaces the splash on screen).  initSize()
     * deliberately SKIPS the framebuffer-mode switch while the splash is up (to
     * keep the boot logo intact during the slow load), so the game's real
     * surface mode is programmed HERE, exactly once, at the splash->game
     * handoff -- this is what lets a 640x480 SCI32 game step the framebuffer up
     * from the 320x240 splash.  The blit just below fills the new surface. */
    if (_splashActive) {
        _splashActive = false;
        configureFramebufferMode(_screenW, _screenH);
        fbReset();   /* mode just changed under us: every buffer is stale -> full blit */
    }

    /* Pump audio before and after presentation; the frame copy/flip can stall
     * long enough to drain the 21 ms audio FIFO.  Advance the MIDI PARSER too,
     * not just the mixer: during a screen transition the engine spins
     * updateScreen() per frame without reaching delayMillis/pollEvent, so
     * without this the held notes keep sounding but the MELODY freezes -- the
     * residual "hiccup" between screens.  The parser is s_inProc-guarded; the
     * old "no MIDI recursion" caveat predates that guard. */
    extern void openfpga_midi_pump_pending(void);
    extern void openfpga_mixer_pump_only(void);
    openfpga_midi_pump_pending();
    openfpga_mixer_pump_only();

    if (_paletteDirty) {
        uint32_t pal32[256];
        for (int i = 0; i < 256; i++) {
            pal32[i] = ((uint32_t)_palette[i*3]     << 16) |
                       ((uint32_t)_palette[i*3 + 1] <<  8) |
                        (uint32_t)_palette[i*3 + 2];
        }
        if (_keypadMode)
            pal32[255] = 0x00FFFFFF; /* reserve white ink for the keypad legend */
        of_video_palette_bulk(pal32, 256);
        _paletteDirty = false;
    }

    uint8_t *fb = acquireFrameBuffer();
    uint fbW, fbH, fbStride;
    getFramebufferMode(fbW, fbH, fbStride);

    int xOff = ((int)fbW - (int)_screenW) / 2;
    int yOff = ((int)fbH - (int)_screenH) / 2;
    if (xOff < 0) xOff = 0;
    if (yOff < 0) yOff = 0;
    uint copyW = _screenW;
    uint copyH = _screenH;
    if (copyW > fbW) copyW = fbW;
    if (copyH > fbH) copyH = fbH;

    if (!s_fbReady) fbReset();
    const int idx = _videoBufIdx;

    /* Current cursor box (surface coords). The cursor is composited onto the
     * framebuffer (not into _screenBuf), so on a partial blit we must refresh
     * the bg under BOTH this buffer's previously-drawn cursor (to erase it) and
     * the new position, and flush those rows. */
    Common::Rect curCursor;
    if (_cursorVisible && _cursorW > 0 && _cursorH > 0) {
        int cx = _cursorX - _cursorHotX;
        int cy = _cursorY - _cursorHotY;
        curCursor = Common::Rect(cx, cy, cx + (int)_cursorW, cy + (int)_cursorH);
        curCursor.clip(Common::Rect(0, 0, (int)copyW, (int)copyH));
    }

    /* Decide full-surface vs bounding-box blit for THIS buffer. */
    bool full = false;
    Common::Rect box;   /* empty == nothing changed since this buffer was drawn */
    if (idx < 0 || idx >= kFbSlots || (_keypadMode && !_splashActive)) {
        full = true;                       /* no GPU buffer / keypad overlay -> whole frame */
    } else {
        const int32 last = s_fbDrawn[idx];
        if (last < 0 || ((int32)s_fbFrame - last) >= kFbHist) {
            full = true;                   /* first use / history too shallow */
        } else {
            for (uint32 f = (uint32)last + 1; f <= s_fbFrame; ++f) {
                const int s = (int)(f % kFbHist);
                if (s_fbFull[s]) { full = true; break; }
                if (!s_fbBox[s].isEmpty()) {
                    if (box.isEmpty()) box = s_fbBox[s]; else box.extend(s_fbBox[s]);
                }
            }
        }
    }

    if (!full) {
        if (idx >= 0 && idx < kFbSlots && !s_fbCursor[idx].isEmpty()) {
            if (box.isEmpty()) box = s_fbCursor[idx]; else box.extend(s_fbCursor[idx]);
        }
        if (!curCursor.isEmpty()) {
            if (box.isEmpty()) box = curCursor; else box.extend(curCursor);
        }
        box.clip(Common::Rect(0, 0, (int)copyW, (int)copyH));
    }

    if (full) {
        clearFrameBorders(fb, fbW, fbH, fbStride, xOff, yOff, copyW, copyH);
        for (uint y = 0; y < copyH; ++y) {
            memcpy(fb + (y + yOff) * fbStride + xOff,
                   _screenBuf + y * _screenW,
                   copyW);
        }
    } else if (!box.isEmpty()) {
        for (int y = box.top; y < box.bottom; ++y) {
            memcpy(fb + (uint)(y + yOff) * fbStride + xOff + box.left,
                   _screenBuf + (uint)y * _screenW + box.left,
                   (uint)box.width());
        }
    }
    /* else: this buffer already holds the current frame -- nothing to copy. */

    if (_cursorVisible)
        drawCursor(fb, fbW, fbH, fbStride, xOff, yOff, copyW, copyH);

    if (_keypadMode && !_splashActive)
        drawKeypadLegend(fb, fbW, fbH, fbStride);

    /* Flush only the rows we touched (full rows of the dirty box; the cursor was
     * folded into the box above).  GPU reads cached SDRAM, so unflushed writes
     * would not be presented. */
    if (full) {
        of_cache_flush_range(fb, fbStride * fbH);
    } else if (!box.isEmpty()) {
        of_cache_flush_range(fb + (uint)(box.top + yOff) * fbStride,
                             (uint)box.height() * fbStride);
    }

    if (idx >= 0 && idx < kFbSlots) {
        s_fbDrawn[idx]  = (int32)s_fbFrame;
        s_fbCursor[idx] = curCursor;
    }

    presentFrame();
    _screenDirty = false;

    /* Advance the frame counter and clear the slot that becomes the new current
     * frame's accumulator (it last held a frame kFbHist ago, now safe to reuse). */
    s_fbFrame++;
    const int ns = (int)(s_fbFrame % kFbHist);
    s_fbBox[ns] = Common::Rect();
    s_fbFull[ns] = false;

    extern void openfpga_midi_pump_pending(void);
    extern void openfpga_mixer_pump_only(void);
    extern void openfpga_audiocd_pump(void);
    openfpga_midi_pump_pending();
    /* Full CDDA pump HERE, right after the flip is queued: the next engine
     * present is at least a frame away, so this is where the batched CD-audio
     * refill (a few large blocking bridge reads, ~5x/s) can run without
     * delaying a frame. */
    openfpga_audiocd_pump();
    openfpga_mixer_pump_only();
}

void OpenFPGAGraphicsManager::presentCursor() {
    /* Nothing to smooth before the first real frame (splash still owns the
     * screen, and a present would prematurely switch the framebuffer mode) or
     * while the cursor is hidden. */
    if (_splashActive || !_cursorVisible)
        return;
    /* If the engine itself presented very recently it is actively redrawing, and
     * its own frame already shows the cursor at its current (serviceInput-moved)
     * position -- adding a second flip here would just double the present rate
     * and starve the audio pump (the talkie-speech stutter).  Only present when
     * the game has gone quiet (static room, or stuck in a long non-yielding
     * load/render), which is exactly when the cursor would otherwise freeze. */
    if ((uint32)(of_time_ms() - _lastEnginePresentMs) < 30u)
        return;
    /* Only the cursor moved, so the dirty-rect blit is just the cursor box. */
    presentFrameInternal();
}

void OpenFPGAGraphicsManager::setKeypadMode(bool on) {
    if (on != _keypadMode) {
        _keypadMode = on;
        /* Re-upload the palette so the reserved white ink (index 255) is
         * asserted on entry and the game's real index 255 restored on exit. */
        _paletteDirty = true;
    }
}

void OpenFPGAGraphicsManager::drawKeypadLegend(uint8_t *fb, uint fbW, uint fbH,
                                               uint fbStride) const {
    const Graphics::Font *font =
        Graphics::FontManager::instance().getFontByUsage(Graphics::FontManager::kConsoleFont);
    if (!font)
        return;

    const int fh = font->getFontHeight();          /* 8 px for the console font */
    const int line2Y = (int)fbH - fh - 1;
    const int line1Y = line2Y - fh;
    if (line1Y < 1)
        return;

    /* Black background strip behind the two rows for legibility. */
    int top = line1Y - 1;
    if (top < 0) top = 0;
    for (int y = top; y < (int)fbH; ++y)
        memset(fb + (uint)y * fbStride, 0, fbW);

    Graphics::Surface surf;
    surf.init((int16)fbW, (int16)fbH, (int16)fbStride, fb,
              Graphics::PixelFormat::createFormatCLUT8());

    static const char *kLine1 =
        "ANSWER: A=a B=b X=c Y=d    digits 1-4=up/dn/lt/rt";
    static const char *kLine2 = "START=Enter   SELECT=exit keypad";

    /* Index 255 is forced to white in updateScreen()'s palette upload. */
    font->drawString(&surf, kLine1, 2, line1Y, (int)fbW, 255, Graphics::kTextAlignLeft);
    font->drawString(&surf, kLine2, 2, line2Y, (int)fbW, 255, Graphics::kTextAlignLeft);
}

void OpenFPGAGraphicsManager::showSplash() {
    /* Render the logo through the SAME path the engine uses for every frame
     * (updateScreen), which is known to apply the palette correctly -- the
     * earlier hand-rolled path showed a yellow screen because the palette did
     * not latch on the very first present after of_gpu_init.  Stage the logo as
     * an 8-bit "screen" + palette, then cycle updateScreen a few times so the
     * palette latches across the triple buffer.  The engine resets the screen
     * size + buffer + palette on its first real frame, replacing this. */
    _screenW = SPLASH_W;
    _screenH = SPLASH_H;

    /* Draw the logo a little smaller than full-screen: nearest-neighbor
     * downscale into a centered region and fill the surround with the logo's
     * background color (top-left corner pixel).  Tweak SPLASH_NUM/DEN to
     * resize -- 4/5 = 80%. */
    const int SPLASH_NUM = 4, SPLASH_DEN = 5;
    const int dstW = SPLASH_W * SPLASH_NUM / SPLASH_DEN;
    const int dstH = SPLASH_H * SPLASH_NUM / SPLASH_DEN;
    const int xOff = (SPLASH_W - dstW) / 2;
    const int yOff = (SPLASH_H - dstH) / 2;
    memset(_screenBuf, SPLASH_PIX[0], SPLASH_W * SPLASH_H);
    for (int dy = 0; dy < dstH; ++dy) {
        const uint8_t *srcRow = SPLASH_PIX + (dy * SPLASH_H / dstH) * SPLASH_W;
        uint8_t *dstRow = _screenBuf + (yOff + dy) * SPLASH_W + xOff;
        for (int dx = 0; dx < dstW; ++dx)
            dstRow[dx] = srcRow[dx * SPLASH_W / dstW];
    }

    for (int i = 0; i < 256; i++) {
        _palette[i * 3]     = (uint8_t)(SPLASH_PAL[i] >> 16);
        _palette[i * 3 + 1] = (uint8_t)(SPLASH_PAL[i] >> 8);
        _palette[i * 3 + 2] = (uint8_t)(SPLASH_PAL[i]);
    }
    for (int n = 0; n < 3; n++) {
        _paletteDirty = true;
        updateScreen();
    }
    /* Armed only after our own updateScreen() calls above, so the next
     * updateScreen() (the engine's first real frame) is what disarms it. */
    _splashActive = true;
}

Graphics::PixelFormat OpenFPGAGraphicsManager::getOverlayFormat() const {
    return Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0);
}

bool OpenFPGAGraphicsManager::showMouse(bool visible) {
    bool prev = _cursorVisible;
    _cursorVisible = visible;
    _screenDirty = true;
    return prev;
}

void OpenFPGAGraphicsManager::warpMouse(int x, int y) {
    _cursorX = x;
    _cursorY = y;
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::setMouseCursor(const void *buf, uint w, uint h,
                                              int hotspotX, int hotspotY,
                                              uint32 keycolor, bool dontScale,
                                              const Graphics::PixelFormat *format,
                                              const byte *mask) {
    _cursorW = (w > 64) ? 64 : w;
    _cursorH = (h > 64) ? 64 : h;
    _cursorHotX = hotspotX;
    _cursorHotY = hotspotY;
    _cursorKeycolor = keycolor;

    const byte *src = (const byte *)buf;
    for (uint row = 0; row < _cursorH; row++)
        memcpy(_cursorData + row * 64, src + row * w, _cursorW);

    _screenDirty = true;
}

void OpenFPGAGraphicsManager::moveMouse(int dx, int dy) {
    _cursorX += dx;
    _cursorY += dy;
    if (_cursorX < 0) _cursorX = 0;
    if (_cursorY < 0) _cursorY = 0;
    if (_cursorX >= (int16)_screenW) _cursorX = _screenW - 1;
    if (_cursorY >= (int16)_screenH) _cursorY = _screenH - 1;
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::drawCursor(uint8_t *dst, uint fbW, uint fbH,
                                         uint fbStride, int xOff, int yOff,
                                         uint clipW, uint clipH) const {
    if (!_cursorVisible || _cursorW == 0 || _cursorH == 0)
        return;

    int cx = _cursorX - _cursorHotX + xOff;
    int cy = _cursorY - _cursorHotY + yOff;

    /* Clip to the game-surface region [xOff, xOff+clipW) x [yOff, yOff+clipH),
     * NOT the full framebuffer: the erase logic in updateScreen restores
     * background from _screenBuf, which only covers the surface.  Pixels
     * painted into the letterbox borders (e.g. a 320x200 game centered in a
     * 320x240 mode) could never be erased -- the "cursor trail at the bottom
     * of the screen" in KQ6.  Also keeps s_fbCursor (surface-clipped) an
     * exact record of what was drawn. */
    int minX = xOff, maxX = xOff + (int)clipW;
    int minY = yOff, maxY = yOff + (int)clipH;
    if (maxX > (int)fbW) maxX = (int)fbW;
    if (maxY > (int)fbH) maxY = (int)fbH;

    for (uint row = 0; row < _cursorH; row++) {
        int sy = cy + (int)row;
        if (sy < minY || sy >= maxY) continue;
        for (uint col = 0; col < _cursorW; col++) {
            int sx = cx + (int)col;
            if (sx < minX || sx >= maxX) continue;
            uint8_t px = _cursorData[row * 64 + col];
            if (px != (_cursorKeycolor & 0xFF))
                dst[sy * fbStride + sx] = px;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * OSystem_OpenFPGA
 * ═══════════════════════════════════════════════════════════════════ */

namespace {

struct PocketKeyBinding {
    bool configured;
    Common::KeyCode keycode;
    uint16 ascii;
};

struct PocketKeyName {
    const char *name;
    Common::KeyCode keycode;
    uint16 ascii;
};

static const PocketKeyName kPocketKeyNames[] = {
    { "enter",     Common::KEYCODE_RETURN,     Common::ASCII_RETURN },
    { "return",    Common::KEYCODE_RETURN,     Common::ASCII_RETURN },
    { "escape",    Common::KEYCODE_ESCAPE,     Common::ASCII_ESCAPE },
    { "esc",       Common::KEYCODE_ESCAPE,     Common::ASCII_ESCAPE },
    { "space",     Common::KEYCODE_SPACE,      Common::ASCII_SPACE },
    { "tab",       Common::KEYCODE_TAB,        Common::ASCII_TAB },
    { "backspace", Common::KEYCODE_BACKSPACE,  Common::ASCII_BACKSPACE },

    { "period",    Common::KEYCODE_PERIOD,      '.' },
    { "dot",       Common::KEYCODE_PERIOD,      '.' },
    { "comma",     Common::KEYCODE_COMMA,       ',' },
    { "minus",     Common::KEYCODE_MINUS,       '-' },
    { "equals",    Common::KEYCODE_EQUALS,      '=' },
    { "slash",     Common::KEYCODE_SLASH,       '/' },
    { "semicolon", Common::KEYCODE_SEMICOLON,   ';' },

    { "delete",    Common::KEYCODE_DELETE,      127 },
    { "insert",    Common::KEYCODE_INSERT,      0 },
    { "home",      Common::KEYCODE_HOME,        0 },
    { "end",       Common::KEYCODE_END,         0 },
    { "pageup",    Common::KEYCODE_PAGEUP,      0 },
    { "pagedown",  Common::KEYCODE_PAGEDOWN,    0 },

    { "up",        Common::KEYCODE_UP,          0 },
    { "down",      Common::KEYCODE_DOWN,        0 },
    { "left",      Common::KEYCODE_LEFT,        0 },
    { "right",     Common::KEYCODE_RIGHT,       0 },

    { "f1",        Common::KEYCODE_F1,          Common::ASCII_F1 },
    { "f2",        Common::KEYCODE_F2,          Common::ASCII_F2 },
    { "f3",        Common::KEYCODE_F3,          Common::ASCII_F3 },
    { "f4",        Common::KEYCODE_F4,          Common::ASCII_F4 },
    { "f5",        Common::KEYCODE_F5,          Common::ASCII_F5 },
    { "f6",        Common::KEYCODE_F6,          Common::ASCII_F6 },
    { "f7",        Common::KEYCODE_F7,          Common::ASCII_F7 },
    { "f8",        Common::KEYCODE_F8,          Common::ASCII_F8 },
    { "f9",        Common::KEYCODE_F9,          Common::ASCII_F9 },
    { "f10",       Common::KEYCODE_F10,         Common::ASCII_F10 },
    { "f11",       Common::KEYCODE_F11,         Common::ASCII_F11 },
    { "f12",       Common::KEYCODE_F12,         Common::ASCII_F12 }
};

static PocketKeyBinding parsePocketKeyBinding(const Common::String &value) {
    PocketKeyBinding binding = {
        false,
        Common::KEYCODE_INVALID,
        0
    };

    Common::String key = value;
    key.toLowercase();
    key.trim();

    if (key.empty() || key == "none")
        return binding;

    const Common::String keyboardPrefix = "keyboard:";
    if (key.hasPrefix(keyboardPrefix))
        key = key.substr(keyboardPrefix.size());

    if (key.size() == 1) {
        const char ch = key[0];

        if (ch >= '0' && ch <= '9') {
            binding.configured = true;
            binding.keycode = static_cast<Common::KeyCode>(
                Common::KEYCODE_0 + (ch - '0'));
            binding.ascii = ch;
            return binding;
        }

        if (ch >= 'a' && ch <= 'z') {
            binding.configured = true;
            binding.keycode = static_cast<Common::KeyCode>(
                Common::KEYCODE_a + (ch - 'a'));
            binding.ascii = ch;
            return binding;
        }

        for (uint i = 0;
             i < sizeof(kPocketKeyNames) / sizeof(kPocketKeyNames[0]);
             ++i) {
            if (key[0] == kPocketKeyNames[i].ascii &&
                kPocketKeyNames[i].ascii != 0) {
                binding.configured = true;
                binding.keycode = kPocketKeyNames[i].keycode;
                binding.ascii = kPocketKeyNames[i].ascii;
                return binding;
            }
        }
    }

    for (uint i = 0;
         i < sizeof(kPocketKeyNames) / sizeof(kPocketKeyNames[0]);
         ++i) {
        if (key == kPocketKeyNames[i].name) {
            binding.configured = true;
            binding.keycode = kPocketKeyNames[i].keycode;
            binding.ascii = kPocketKeyNames[i].ascii;
            return binding;
        }
    }

    return binding;
}

static PocketKeyBinding getPocketKeyBinding(const char *configKey) {
    if (!ConfMan.hasKey(configKey))
        return parsePocketKeyBinding(Common::String());

    return parsePocketKeyBinding(ConfMan.get(configKey));
}

struct PocketControlConfig {
    bool loaded;

    PocketKeyBinding selectUp;
    PocketKeyBinding selectDown;
    PocketKeyBinding selectLeft;
    PocketKeyBinding selectRight;

    PocketKeyBinding selectA;
    PocketKeyBinding selectB;
    PocketKeyBinding selectX;
    PocketKeyBinding selectY;
    PocketKeyBinding selectStart;

    PocketKeyBinding l;
    PocketKeyBinding r;

    PocketKeyBinding start;
    PocketKeyBinding a;
    PocketKeyBinding b;
    PocketKeyBinding x;
    PocketKeyBinding y;
};

static PocketControlConfig gPocketControls = {};

static void loadPocketControlConfig() {
    if (gPocketControls.loaded)
        return;

    gPocketControls.selectUp =
        getPocketKeyBinding("openfpga_control_select_up");
    gPocketControls.selectDown =
        getPocketKeyBinding("openfpga_control_select_down");
    gPocketControls.selectLeft =
        getPocketKeyBinding("openfpga_control_select_left");
    gPocketControls.selectRight =
        getPocketKeyBinding("openfpga_control_select_right");

    gPocketControls.selectA =
        getPocketKeyBinding("openfpga_control_select_a");
    gPocketControls.selectB =
        getPocketKeyBinding("openfpga_control_select_b");
    gPocketControls.selectX =
        getPocketKeyBinding("openfpga_control_select_x");
    gPocketControls.selectY =
        getPocketKeyBinding("openfpga_control_select_y");
    gPocketControls.selectStart =
        getPocketKeyBinding("openfpga_control_select_start");

    gPocketControls.l =
        getPocketKeyBinding("openfpga_control_l");
    gPocketControls.r =
        getPocketKeyBinding("openfpga_control_r");

    gPocketControls.start =
        getPocketKeyBinding("openfpga_control_start");
    gPocketControls.a =
        getPocketKeyBinding("openfpga_control_a");
    gPocketControls.b =
        getPocketKeyBinding("openfpga_control_b");
    gPocketControls.x =
        getPocketKeyBinding("openfpga_control_x");
    gPocketControls.y =
        getPocketKeyBinding("openfpga_control_y");

    gPocketControls.loaded = true;
}

int pixelsFromRate(int rate, uint32 elapsedMs, int32 &accum) {
    if (rate == 0) {
        accum = 0;
        return 0;
    }

    if ((rate > 0 && accum < 0) || (rate < 0 && accum > 0))
        accum = 0;

    accum += rate * (int32)elapsedMs;
    int pixels = accum / 1000;
    accum -= pixels * 1000;
    return pixels;
}

} // namespace

OSystem_OpenFPGA::OSystem_OpenFPGA()
    : _startTime(0), _ofGfx(nullptr),
      _mouseButtonL(false), _mouseButtonR(false), _autoDismissCounter(60),
      _ignoreInitialButtons(true), _keypadMode(false), _dockKbArmed(false),
      _copyProtectActive(false), _copyProtectKeys(0),
      _masterVolume(255), _musicVolume(192),
      _selectHeld(false), _selectConsumed(false),
      _lastMouseTick(0),
      _mouseAccumX(0), _mouseAccumY(0),
      _lastSyncedMouseX(-1), _lastSyncedMouseY(-1),
      _joyFiltX(0), _joyFiltY(0),
      _lastCursorServiceMs(0), _inCursorService(false) {
}

OSystem_OpenFPGA::~OSystem_OpenFPGA() {
}

/* Stashed OSystem instance so the free-function work pumps (openfpga_pump_during_
 * load, called from the FS read path) can service the cursor without a header
 * dependency.  Set in initBackend(). */
static OSystem_OpenFPGA *g_ofCursorSystem = nullptr;

void OSystem_OpenFPGA::initBackend() {
    /* Guard against double init (main calls us, then scummvm_main calls us again) */
    if (_ofGfx)
        return;

    g_ofCursorSystem = this;

    _startTime = of_time_ms();

    _ofGfx = new OpenFPGAGraphicsManager();
    _graphicsManager = _ofGfx;

    _mixerManager = new OpenFPGAMixerManager();
    _mixerManager->init();

    _timerManager = new DefaultTimerManager();
    _eventManager = new DefaultEventManager(this);
    _savefileManager = new OpenFPGASaveFileManager();
    _fsFactory = new OpenFPGAFilesystemFactory();

    openfpga_set_pump_managers(_timerManager, _mixerManager);

    /* Pre-set the AudioCD manager so BaseBackend::initBackend() skips
     * its DefaultAudioCDManager lazy-init.  Ours additionally streams
     * raw CDDA from a .cue/.bin pair in SearchMan -- main.cpp adds
     * the game zip before initBackend is called, so the cue is visible. */
    _audiocdManager = new OpenFPGAAudioCDManager();

    BaseBackend::initBackend();
}

// Map a USB-HID Keyboard/Keypad page (0x07) usage code to a ScummVM keycode +
// ASCII, honoring Shift for printable keys.  Returns false for usages we don't
// map (modifier keys, etc.).  Drives the dock keyboard in pollEvent().
static bool hidUsageToScummVM(uint8_t usage, bool shift,
                              Common::KeyCode &kc, uint16 &ascii) {
    char un = 0, sh = 0; // printable unshifted / shifted char (0 = not printable)

    if (usage >= 0x04 && usage <= 0x1D) {            // a..z
        un = (char)('a' + (usage - 0x04));
        sh = (char)('A' + (usage - 0x04));
    } else if (usage >= 0x1E && usage <= 0x27) {     // 1..0 number row
        static const char row[] = "1234567890";
        static const char sym[] = "!@#$%^&*()";
        un = row[usage - 0x1E];
        sh = sym[usage - 0x1E];
    } else if (usage >= 0x59 && usage <= 0x61) {     // keypad 1..9 -> plain digits
        un = sh = (char)('1' + (usage - 0x59));
    } else {
        switch (usage) {
        case 0x2D: un = '-';  sh = '_';  break;
        case 0x2E: un = '=';  sh = '+';  break;
        case 0x2F: un = '[';  sh = '{';  break;
        case 0x30: un = ']';  sh = '}';  break;
        case 0x31: un = '\\'; sh = '|';  break;
        case 0x33: un = ';';  sh = ':';  break;
        case 0x34: un = '\''; sh = '"';  break;
        case 0x35: un = '`';  sh = '~';  break;
        case 0x36: un = ',';  sh = '<';  break;
        case 0x37: un = '.';  sh = '>';  break;
        case 0x38: un = '/';  sh = '?';  break;
        case 0x2C: un = sh = ' '; break;             // Space
        case 0x62: un = sh = '0'; break;             // keypad 0
        case 0x54: un = sh = '/'; break;             // keypad / * - + .
        case 0x55: un = sh = '*'; break;
        case 0x56: un = sh = '-'; break;
        case 0x57: un = sh = '+'; break;
        case 0x63: un = sh = '.'; break;
        default: break;
        }
    }

    if (un) {
        ascii = (uint16)(uint8_t)(shift ? sh : un);
        kc = (Common::KeyCode)(uint8_t)un;           // keycode = the physical (unshifted) key
        return true;
    }

    switch (usage) {                                 // non-printable keys
    case 0x28: case 0x58: kc = Common::KEYCODE_RETURN;    ascii = Common::ASCII_RETURN; return true;
    case 0x29:            kc = Common::KEYCODE_ESCAPE;    ascii = 27;  return true;
    case 0x2A:            kc = Common::KEYCODE_BACKSPACE; ascii = 8;   return true;
    case 0x2B:            kc = Common::KEYCODE_TAB;       ascii = 9;   return true;
    case 0x4F:            kc = Common::KEYCODE_RIGHT;     ascii = 0;   return true;
    case 0x50:            kc = Common::KEYCODE_LEFT;      ascii = 0;   return true;
    case 0x51:            kc = Common::KEYCODE_DOWN;      ascii = 0;   return true;
    case 0x52:            kc = Common::KEYCODE_UP;        ascii = 0;   return true;
    case 0x49:            kc = Common::KEYCODE_INSERT;    ascii = 0;   return true;
    case 0x4A:            kc = Common::KEYCODE_HOME;      ascii = 0;   return true;
    case 0x4B:            kc = Common::KEYCODE_PAGEUP;    ascii = 0;   return true;
    case 0x4C:            kc = Common::KEYCODE_DELETE;    ascii = 127; return true;
    case 0x4D:            kc = Common::KEYCODE_END;       ascii = 0;   return true;
    case 0x4E:            kc = Common::KEYCODE_PAGEDOWN;  ascii = 0;   return true;
    default: break;
    }
    if (usage >= 0x3A && usage <= 0x45) {            // F1..F12
        kc = (Common::KeyCode)(Common::KEYCODE_F1 + (usage - 0x3A));
        ascii = 0;
        return true;
    }
    return false;
}

bool OSystem_OpenFPGA::pollEvent(Common::Event &event) {
    openfpga_drive_audio_and_timers();

    if (popQueuedEvent(event))
        return true;

    /* All input is read in serviceInput() (also called from delayMillis() to
     * keep the cursor smooth); it pushes events onto _eventQueue, which we then
     * drain.  Keeping of_input_poll() to this one funnel is what stops the
     * delayMillis poll from stealing button-press edges from us. */
    serviceInput(false);
    /* Cursor motion may have been applied without an event (delayMillis
     * passes only move + present); queue a catch-up MOUSEMOVE if the
     * engine's last-seen position is stale. */
    syncEngineMousePos();
    return popQueuedEvent(event);
}

void OSystem_OpenFPGA::serviceInput(bool fromDelay) {
    of_input_poll();

    /* Auto-dismiss REMOVED: this fired a single blind Return ~60 polls (~2s)
     * after startup to clear ScummVM's "unknown game version" dialog.  But this
     * port forces openfpga_skip_detection, so that dialog never appears -- and
     * the stray Return instead landed mid-intro (~2.6s, during room 120/130)
     * and SKIPPED it, since SCI/AGI/SCUMM intros abort on any keypress.  (The
     * diagnostic "[ofev] chr=d" was 0x0d = Return, NOT the letter 'd'.)  If a
     * future game needs a startup dialog dismissed, gate a Return on the GUI
     * actually being active rather than firing it blindly on a timer. */

    /* ===== Dock USB keyboard =====
     * Only serviced from pollEvent (fromDelay=false).  The keyboard is read
     * through its own syscall (not of_input_poll) and the read consumes the
     * key edges; unlike the mouse below, not every consumed edge turns into a
     * queued event (the arming filter and the one-key-per-pass early return
     * below), so the extra delayMillis service passes would silently drop
     * typed characters -- and typing gains nothing from 60 Hz servicing.
     *
     * A real keyboard plugged into the Pocket dock types directly into the
     * engine -- no keypad mode needed (so e.g. the LSL age quiz, which reads a
     * typed number via kReadNumber, is answerable normally).  Checked before
     * the controller so typed input is never swallowed by keypad/mouse logic. */
    if (!fromDelay) {
        of_keyboard_state_t kb;
        of_input_keyboard_state(&kb);
        if (kb.present) {
            byte flags = 0;
            if (kb.modifiers & (OF_KEYMOD_LCTRL | OF_KEYMOD_RCTRL)) flags |= Common::KBD_CTRL;
            if (kb.modifiers & (OF_KEYMOD_LALT  | OF_KEYMOD_RALT))  flags |= Common::KBD_ALT;
            const bool shift = (kb.modifiers & (OF_KEYMOD_LSHIFT | OF_KEYMOD_RSHIFT)) != 0;
            if (shift)
                flags |= Common::KBD_SHIFT;
            /* Only trust the dock keyboard once it has reported an all-keys-
             * released frame at least once -- a safety net against a stuck or
             * garbage key present from boot (or a held launch key) before we
             * accept input, while still letting a real, idle-at-boot keyboard
             * work normally.  Usages 0x00-0x03 are no-event/error codes, never
             * real keys, so we skip them. */
            bool anyPressed = false;
            for (uint u = 0x04; u < OF_KEYBOARD_MAX_USAGE; ++u) {
                if (of_keyboard_key_pressed(&kb, (uint8_t)u)) {
                    anyPressed = true;
                    if (_dockKbArmed) {
                        Common::KeyCode kc;
                        uint16 ascii;
                        if (hidUsageToScummVM((uint8_t)u, shift, kc, ascii)) {
                            queueKey(kc, ascii, flags);
                            return;
                        }
                    }
                }
            }
            if (!anyPressed)
                _dockKbArmed = true;
        }
    }

    /* ===== Dock USB mouse =====
     * Relative motion -> cursor, buttons -> left/right click.  Serviced on
     * BOTH passes, unlike the keyboard: the consuming read is safe here
     * because motion is applied to the cursor the moment it is read and
     * button edges are queued the moment they are read -- the same property
     * that makes the controller safe on both passes.  Servicing from
     * delayMillis is what keeps a physical mouse as smooth as the pad cursor
     * while the engine isn't redrawing (static rooms, script waits, loads).
     * Motion is applied BEFORE the buttons so a click's same-poll motion
     * isn't dropped and the click lands at the moved cursor. */
    bool dockMoved = false;
    {
        of_mouse_state_t m;
        of_input_mouse_state(&m);
        if (m.present && (m.dx || m.dy)) {
            /* The dock reports very large, bursty deltas (observed up to
             * ~5000/poll), and moveMouse moves in game pixels -- so cap the
             * per-poll input (limits a single jump to CAP/DIV px) and scale
             * down by DOCK_MOUSE_DIV, accumulating the remainder so slow
             * movements still track.  Raise DOCK_MOUSE_DIV to slow it. */
            static const int DOCK_MOUSE_DIV = 128;
            static const int DOCK_MOUSE_CAP = 4096;   /* => max ~32 px/poll */
            static int accX = 0, accY = 0;
            int rx = (int)m.dx, ry = (int)m.dy;
            if (rx >  DOCK_MOUSE_CAP) rx =  DOCK_MOUSE_CAP;
            if (rx < -DOCK_MOUSE_CAP) rx = -DOCK_MOUSE_CAP;
            if (ry >  DOCK_MOUSE_CAP) ry =  DOCK_MOUSE_CAP;
            if (ry < -DOCK_MOUSE_CAP) ry = -DOCK_MOUSE_CAP;
            accX += rx;
            accY += ry;
            int mdx = accX / DOCK_MOUSE_DIV;
            int mdy = accY / DOCK_MOUSE_DIV;
            accX -= mdx * DOCK_MOUSE_DIV;
            accY -= mdy * DOCK_MOUSE_DIV;
            if (mdx || mdy) {
                _ofGfx->moveMouse(mdx, mdy);
                dockMoved = true;
            }
        }
        /* Button edges are handled even when !present: on hot-unplug the
         * firmware latches release edges for anything still held into this
         * final read, and dropping them would leave the engine with a button
         * stuck down forever. */
        struct { uint16 mask; Common::EventType down, up; } mb[] = {
            { 0x1u, Common::EVENT_LBUTTONDOWN, Common::EVENT_LBUTTONUP },
            { 0x2u, Common::EVENT_RBUTTONDOWN, Common::EVENT_RBUTTONUP },
        };
        for (uint i = 0; i < 2; ++i) {
            const bool down = (m.buttons_pressed  & mb[i].mask) != 0;
            const bool up   = (m.buttons_released & mb[i].mask) != 0;
            if (down && up) {
                /* Both edges in one poll: order by the final level, so a
                 * sub-frame click ends UP and a release+re-press ends DOWN. */
                if (m.buttons & mb[i].mask) {
                    queueMouseEvent(mb[i].up);
                    queueMouseEvent(mb[i].down);
                } else {
                    queueMouseEvent(mb[i].down);
                    queueMouseEvent(mb[i].up);
                }
            } else if (down) {
                queueMouseEvent(mb[i].down);
            } else if (up) {
                queueMouseEvent(mb[i].up);
            }
        }
        /* From delayMillis the engine isn't redrawing -- present the moved
         * cursor ourselves, exactly like the pad path below.  Done here, not
         * there, so mouse motion still presents when SELECT/keypad handling
         * returns before reaching the pad cursor code. */
        if (fromDelay && dockMoved)
            _ofGfx->presentCursor();
    }

    /* Analog stick and D-pad both drive the mouse.  Movement is applied
     * before button handling so clicks land at the current cursor, but
     * button/key events are returned first so continuous movement cannot
     * starve clicks or shortcuts. */
    of_input_state_t state;
    of_input_state(0, &state);

    loadPocketControlConfig();

    if (_ignoreInitialButtons) {
        if (state.buttons != 0) {
            _lastMouseTick = getMillis();
            _mouseAccumX = 0;
            _mouseAccumY = 0;
            _mouseButtonL = false;
            _mouseButtonR = false;
            return;
        }
        _ignoreInitialButtons = false;
    }

    /* NOTE: no copy-protection auto-bypass here.  The SCUMM v5 copy-protection
     * screen (MI2 room 108, Atlantis, ...) reads its answer from the MOUSE
     * CURSOR POSITION, not the keyboard (see engines/scumm/script_v5.cpp
     * o5_stringOps -- "read from a negative index if the mouse cursor is moved
     * to the top").  An earlier version auto-TYPED digits here and returned
     * early every poll, which SWALLOWED mouse movement -> the cursor froze and
     * looked "not visible", making the screen impossible.  So we let normal
     * input through: move the cursor (stick/D-pad), click symbols with A.  The
     * check is disabled (copy_protection=false), so any answer passes -- just
     * enter something and confirm.  (_copyProtectActive is still latched in
     * setFeatureState for diagnostics; keypad mode via SELECT remains available
     * if a screen does want typed digits.) */

    /* SELECT is a hold-modifier:
     *   SELECT + D-pad Up/Down    -> master output volume up/down (live).
     *   SELECT + D-pad Left/Right -> music volume down/up (live).
     *     These cover the gap left by the SCUMM v5 in-game menu, which only has
     *     Save/Load/Play/Quit (no volume sliders).
     *   SELECT tapped (released with no volume change) -> toggle the
     *     controller numeric keypad, a keyboard-less way to type codes
     *     (e.g. if the copy-protection auto-bypass above is ever capped out):
     *       Up=1 Down=2 Left=3 Right=4  A=5 B=6 X=7 Y=8  L1=9 R1=0
     *       START=Enter   SELECT=exit
     * While SELECT is held, other input is suspended. */
    if (state.buttons & OF_BTN_SELECT) {
        if ((state.buttons_pressed & OF_BTN_LEFT) &&
            gPocketControls.selectLeft.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectLeft.keycode,
                gPocketControls.selectLeft.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_UP) &&
            gPocketControls.selectUp.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectUp.keycode,
                gPocketControls.selectUp.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_DOWN) &&
            gPocketControls.selectDown.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectDown.keycode,
                gPocketControls.selectDown.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_RIGHT) &&
            gPocketControls.selectRight.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectRight.keycode,
                gPocketControls.selectRight.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_A) &&
            gPocketControls.selectA.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectA.keycode,
                gPocketControls.selectA.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_B) &&
            gPocketControls.selectB.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectB.keycode,
                gPocketControls.selectB.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_X) &&
            gPocketControls.selectX.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectX.keycode,
                gPocketControls.selectX.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_Y) &&
            gPocketControls.selectY.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectY.keycode,
                gPocketControls.selectY.ascii);
            return;
        }

        if ((state.buttons_pressed & OF_BTN_START) &&
            gPocketControls.selectStart.configured) {
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(
                gPocketControls.selectStart.keycode,
                gPocketControls.selectStart.ascii);
            return;
        }

        if (!gPocketControls.selectUp.configured &&
            (state.buttons_pressed & OF_BTN_UP)) {
            _masterVolume = (_masterVolume + 16 > 255) ? 255 : _masterVolume + 16;
            of_mixer_set_master_volume(_masterVolume);
            _selectConsumed = true;
        } else if (!gPocketControls.selectDown.configured &&
            (state.buttons_pressed & OF_BTN_DOWN)) {
            _masterVolume = (_masterVolume - 16 < 0) ? 0 : _masterVolume - 16;
            of_mixer_set_master_volume(_masterVolume);
            _selectConsumed = true;
        } else if (
            ((!gPocketControls.selectLeft.configured &&
            (state.buttons_pressed & OF_BTN_LEFT)) ||
            (!gPocketControls.selectRight.configured &&
            (state.buttons_pressed & OF_BTN_RIGHT)))) {
            int d = (state.buttons_pressed & OF_BTN_RIGHT) ? 16 : -16;
            _musicVolume += d;
            if (_musicVolume < 0)   _musicVolume = 0;
            if (_musicVolume > 255) _musicVolume = 255;
            /* CD music (MI1) flows through ScummVM's kMusicSoundType; MIDI
             * music (MI2) through the of_mixer MUSIC group.  Adjust both so
             * "music volume" works regardless of which a game uses. */
            if (getMixer())
                getMixer()->setVolumeForSoundType(Audio::Mixer::kMusicSoundType, _musicVolume);
            of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, _musicVolume);
            _selectConsumed = true;
        } else if (!gPocketControls.selectB.configured &&
            (state.buttons_pressed & OF_BTN_B)) {
            /* SELECT+B = Escape.  SCI menus and many message/dialog windows
             * dismiss/continue on Esc, and we otherwise have no Escape binding
             * (plain Esc is avoided because SCUMM uses it to skip cutscenes).
             * Mark SELECT consumed so the release doesn't toggle the keypad. */
            _selectConsumed = true;
            _selectHeld = true;
            queueKey(Common::KEYCODE_ESCAPE, 27);
            return;
        } else if (!gPocketControls.selectStart.configured &&
            (state.buttons_pressed & OF_BTN_START)) {
            /* SELECT+START = toggle the numeric keypad, deliberately.  Keypad
             * mode disables the mouse and turns every button into a digit with
             * no on-screen indicator, so a bare SELECT tap must NOT enter it
             * (that looked exactly like a hang -- frozen cursor, buttons typing
             * numbers).  Entering is now an explicit two-button gesture. */
            _keypadMode = !_keypadMode;
            _ofGfx->setKeypadMode(_keypadMode);
            _selectConsumed = true;
        }
        _selectHeld = true;
        return;
    }
    if (_selectHeld) {                  /* SELECT just released */
        _selectHeld = false;
        /* A bare SELECT tap only EXITS the keypad (easy escape if it was ever
         * entered); it never enters it.  Entry is SELECT+START (above). */
        if (!_selectConsumed && _keypadMode) {
            _keypadMode = false;
            _ofGfx->setKeypadMode(_keypadMode);
        }
        _selectConsumed = false;
        return;
    }
    if (_keypadMode) {
        static const struct { uint32 btn; Common::KeyCode kc; uint16 ch; } kp[] = {
            { OF_BTN_UP,    Common::KEYCODE_1, '1' }, { OF_BTN_DOWN,  Common::KEYCODE_2, '2' },
            { OF_BTN_LEFT,  Common::KEYCODE_3, '3' }, { OF_BTN_RIGHT, Common::KEYCODE_4, '4' },
            /* Face buttons are letters a-d: SCI multiple-choice quizzes (the
             * LSL age questions) read a letter keypress, not a digit/click. */
            { OF_BTN_A,     Common::KEYCODE_a, 'a' }, { OF_BTN_B,     Common::KEYCODE_b, 'b' },
            { OF_BTN_X,     Common::KEYCODE_c, 'c' }, { OF_BTN_Y,     Common::KEYCODE_d, 'd' },
            { OF_BTN_L1,    Common::KEYCODE_9, '9' }, { OF_BTN_R1,    Common::KEYCODE_0, '0' },
        };
        for (uint i = 0; i < sizeof(kp) / sizeof(kp[0]); ++i) {
            if (state.buttons_pressed & kp[i].btn) {
                queueKey(kp[i].kc, kp[i].ch);
                return;
            }
        }
        if (state.buttons_pressed & OF_BTN_START) {
            /* Enter, but STAY in keypad mode so a sequence of typed values can
             * be entered (e.g. the LSL age quiz reads a typed number per
             * question via kReadNumber: type digit -> Enter -> read result ->
             * Enter -> next question).  Tap SELECT to leave the keypad when done
             * (that restores the mouse). */
            queueKey(Common::KEYCODE_RETURN, Common::ASCII_RETURN);
            return;
        }
        return;   /* swallow movement/clicks while in keypad mode */
    }
    if ((state.buttons_pressed & OF_BTN_L1) &&
        gPocketControls.l.configured) {
        queueKey(
            gPocketControls.l.keycode,
            gPocketControls.l.ascii);
        return;
    }

    if ((state.buttons_pressed & OF_BTN_R1) &&
        gPocketControls.r.configured) {
        queueKey(
            gPocketControls.r.keycode,
            gPocketControls.r.ascii);
        return;
    }

    uint32 nowMs = getMillis();
    uint32 elapsedMs = (_lastMouseTick == 0) ? 16 : nowMs - _lastMouseTick;
    _lastMouseTick = nowMs;
    if (elapsedMs > 50)
        elapsedMs = 50;

    const int deadzone = 4000;
    const bool slowMouse =
        !gPocketControls.l.configured &&
        (state.buttons & (OF_BTN_L1 | OF_BTN_L2)) != 0;

    const bool fastMouse =
        !gPocketControls.r.configured &&
        (state.buttons & (OF_BTN_R1 | OF_BTN_R2)) != 0;

    int analogMaxRate = slowMouse ? 120 : (fastMouse ? 420 : 260);
    int dpadBaseRate = slowMouse ? 60 : (fastMouse ? 240 : 120);
    /* Cursor rates are in GAME pixels/sec.  A hi-res SCI32 surface (640x480) is
     * twice as wide as the 320-px baseline, so a fixed rate crosses it only half
     * as fast -- the cursor feels sluggish.  Scale the rate up with the surface
     * width so it traverses the screen in the same wall-clock time at any
     * resolution (no change at 320; ~2x at 640).  The L/R slow/fast modifiers
     * above still apply on top. */
    const int surfaceW = _ofGfx ? (int)_ofGfx->getWidth() : 320;
    if (surfaceW > 320) {
        analogMaxRate = analogMaxRate * surfaceW / 320;
        dpadBaseRate  = dpadBaseRate  * surfaceW / 320;
    }
    /* Low-pass the raw axes (one-pole IIR, alpha = 1/4) before the deadzone, so
     * ADC jitter on a held stick averages out to a steady value instead of a
     * trembling one -- a chief cause of the cursor "jumping around".  Residual
     * (<=3 counts after decay) sits well inside the deadzone, so a released
     * stick still reads a hard zero with no drift. */
    _joyFiltX += (state.joy_lx - _joyFiltX) / 4;
    _joyFiltY += (state.joy_ly - _joyFiltY) / 4;
    int lx = _joyFiltX;
    int ly = _joyFiltY;
    if (lx > -deadzone && lx < deadzone) lx = 0;
    if (ly > -deadzone && ly < deadzone) ly = 0;

    int rateX = 0;
    int rateY = 0;
    if (lx != 0)
        rateX += (lx * analogMaxRate) / 32767;
    if (ly != 0)
        rateY += (ly * analogMaxRate) / 32767;

    int dpadX = 0;
    int dpadY = 0;
    if (state.buttons & OF_BTN_LEFT)  --dpadX;
    if (state.buttons & OF_BTN_RIGHT) ++dpadX;
    if (state.buttons & OF_BTN_UP)    --dpadY;
    if (state.buttons & OF_BTN_DOWN)  ++dpadY;
    if (dpadX || dpadY) {
        int dpadRate = (dpadX && dpadY) ? ((dpadBaseRate * 3) / 4) : dpadBaseRate;
        rateX += dpadX * dpadRate;
        rateY += dpadY * dpadRate;
    }

    int dx = pixelsFromRate(rateX, elapsedMs, _mouseAccumX);
    int dy = pixelsFromRate(rateY, elapsedMs, _mouseAccumY);

    bool moved = dx != 0 || dy != 0;
    if (moved)
        _ofGfx->moveMouse(dx, dy);

    /* When servicing from delayMillis() the engine isn't redrawing, so present
     * the cursor ourselves -- this is what decouples cursor smoothness from the
     * game's (low, irregular) frame rate.  presentCursor() no-ops if the cursor
     * is hidden or the splash is still up.  Skipped if the dock mouse already
     * presented this pass (its block above): one flip per pass is enough. */
    if (fromDelay && moved && !dockMoved)
        _ofGfx->presentCursor();

    /* A defaults to left mouse click unless overridden by [controls]. */
    if (gPocketControls.a.configured) {
        if (state.buttons_pressed & OF_BTN_A) {
            queueKey(
                gPocketControls.a.keycode,
                gPocketControls.a.ascii);
            return;
        }
    } else {
        const bool aDown = (state.buttons & OF_BTN_A) != 0;
        if (aDown != _mouseButtonL) {
            _mouseButtonL = aDown;
            queueMouseEvent(
                aDown ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP);
            return;
        }
    }

    /* B defaults to right mouse click unless overridden by [controls]. */
    if (gPocketControls.b.configured) {
        if (state.buttons_pressed & OF_BTN_B) {
            queueKey(
                gPocketControls.b.keycode,
                gPocketControls.b.ascii);
            return;
        }
    } else {
        const bool bDown = (state.buttons & OF_BTN_B) != 0;
        if (bDown != _mouseButtonR) {
            _mouseButtonR = bDown;
            queueMouseEvent(
                bDown ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP);
            return;
        }
    }

    /* Controller shortcuts, overridable per game through [controls]. */
    if (state.buttons_pressed & OF_BTN_X) {
        if (gPocketControls.x.configured) {
            queueKey(
                gPocketControls.x.keycode,
                gPocketControls.x.ascii);
        } else {
            queueKey(Common::KEYCODE_RETURN, Common::ASCII_RETURN);
        }
        return;
    }

    if (state.buttons_pressed & OF_BTN_Y) {
        if (gPocketControls.y.configured) {
            queueKey(
                gPocketControls.y.keycode,
                gPocketControls.y.ascii);
        } else {
            queueKey(Common::KEYCODE_SPACE, Common::ASCII_SPACE);
        }
        return;
    }

    if (state.buttons_pressed & OF_BTN_START) {
        if (gPocketControls.start.configured) {
            queueKey(
                gPocketControls.start.keycode,
                gPocketControls.start.ascii);
        } else {
            queueKey(Common::KEYCODE_F5, Common::ASCII_F5);
        }
        return;
    }

    /* Plain Select intentionally has no Escape binding. SCUMM uses Escape
     * as cutscene-abort, so mapping Select there skips to the next scene. */

    if (state.buttons_pressed & OF_BTN_L3) {
        queueKey(Common::KEYCODE_TAB, Common::ASCII_TAB);
        return;
    }

    if (state.buttons_pressed & OF_BTN_R3) {
        queueKey(Common::KEYCODE_PERIOD, '.');
        return;
    }

    /* Pure movement (pad or dock mouse) reaches the engine's hover/verb logic
     * via syncEngineMousePos() in pollEvent(): it queues one catch-up
     * EVENT_MOUSEMOVE whenever the engine's last-seen position is stale.
     * Position-based rather than moved-this-pass so motion applied on
     * delayMillis passes -- where we only move + present -- is never lost,
     * even when this function early-returns above. */
}

/* Queue a catch-up EVENT_MOUSEMOVE if the engine hasn't seen the cursor's
 * current position.  Every queued mouse event stamps the live position (see
 * queueMouseEvent), so this stays one event per actual change, not a flood. */
void OSystem_OpenFPGA::syncEngineMousePos() {
    if (!_ofGfx)
        return;
    if (_ofGfx->getMouseX() != _lastSyncedMouseX || _ofGfx->getMouseY() != _lastSyncedMouseY)
        queueMouseEvent(Common::EVENT_MOUSEMOVE);
}

void OSystem_OpenFPGA::serviceCursorFromWork() {
    if (_inCursorService)               /* re-entered via mixer->FS-load pump */
        return;
    if (g_pumpBusy)                     /* inside an audio pump's FS read -- no
                                         * cursor GPU flip mid-mix; the caller's
                                         * own delayMillis tick services it */
        return;
    uint32 now = of_time_ms();
    if ((uint32)(now - _lastCursorServiceMs) < 16u)   /* ~60 Hz */
        return;
    _lastCursorServiceMs = now;
    _inCursorService = true;
    serviceInput(true);
    _inCursorService = false;
}

void openfpga_service_cursor(void) {
    if (g_ofCursorSystem)
        g_ofCursorSystem->serviceCursorFromWork();
}

void OSystem_OpenFPGA::queueMouseEvent(Common::EventType type) {
    Common::Event ev;
    ev.type = type;
    ev.mouse.x = _ofGfx->getMouseX();
    ev.mouse.y = _ofGfx->getMouseY();
    /* Every mouse event carries the live position, so it also brings the
     * engine up to date -- record that for syncEngineMousePos(). */
    _lastSyncedMouseX = ev.mouse.x;
    _lastSyncedMouseY = ev.mouse.y;
    _eventQueue.push(ev);
}

void OSystem_OpenFPGA::queueKey(Common::KeyCode keycode, uint16 ascii, byte flags) {
    Common::Event ev;
    ev.type = Common::EVENT_KEYDOWN;
    ev.kbdRepeat = false;
    ev.kbd.keycode = keycode;
    ev.kbd.ascii = ascii;
    ev.kbd.flags = flags;
    _eventQueue.push(ev);

    ev.type = Common::EVENT_KEYUP;
    ev.kbdRepeat = false;
    _eventQueue.push(ev);
}

bool OSystem_OpenFPGA::popQueuedEvent(Common::Event &event) {
    if (_eventQueue.empty())
        return false;
    event = _eventQueue.pop();
    return true;
}

void OSystem_OpenFPGA::setFeatureState(Feature f, bool enable) {
    /* The SCUMM engine raises kFeatureVirtualKeyboard exactly while MI2's
     * room-108 copy-protection screen is on (engines/scumm/room.cpp).  We
     * latch it so pollEvent can auto-type past the screen.  Resetting the
     * key counter re-arms the auto-bypass each time the screen appears. */
    if (f == kFeatureVirtualKeyboard) {
        _copyProtectActive = enable;
        _copyProtectKeys = 0;
        warning("[cprot] copy-protection screen %s (room 108 detected)",
                enable ? "ENTERED -> auto-fill armed, press A to submit" : "left");
        return;
    }
    ModularGraphicsBackend::setFeatureState(f, enable);
}

bool OSystem_OpenFPGA::getFeatureState(Feature f) {
    /* Report the latched virtual-keyboard state so room.cpp's
     * "leaving room 108 -> turn it back off" path actually fires (the old
     * stub returned false, so the engine never cleared the flag). */
    if (f == kFeatureVirtualKeyboard)
        return _copyProtectActive;
    return ModularGraphicsBackend::getFeatureState(f);
}

void openfpga_show_splash(void) {
    if (g_splashGfx)
        g_splashGfx->showSplash();
}

Common::MutexInternal *OSystem_OpenFPGA::createMutex() {
    /* Single-threaded port: a real mutex is unnecessary.  Timer-callback
     * re-entrancy (the only concurrency hazard, since the SCI MIDI timer is
     * pumped on the main thread) is handled by the s_inProc guard in
     * openfpga_midi_pump_pending(), not here -- gating the pump on a held
     * mutex starved sound/cue completion and hung SCI games. */
    return new NullMutexInternal();
}

uint32 OSystem_OpenFPGA::getMillis(bool skipRecord) {
    return of_time_ms() - _startTime;
}

/* One guarded audio-servicing tick for delayMillis()'s low-latency loop
 * (g_pumpBusy declared near the top of the file). */
static void pumpAudioTick(OpenFPGAMixerManager *mgr, bool doUpdate) {
    if (g_pumpBusy)
        return;                 /* nested FS-read pump -- the outer tick covers it */
    g_pumpBusy = true;
    extern void openfpga_midi_pump_pending(void);
    extern void openfpga_audiocd_pump(void);
    openfpga_midi_pump_pending();
    openfpga_audiocd_pump();
    if (doUpdate)
        mgr->update();          /* mix + push a block to the HW ring (may read FS) */
    mgr->drain();               /* advance SDK software-mixer voices (cheap) */
    g_pumpBusy = false;
}

void OSystem_OpenFPGA::delayMillis(uint msecs) {
    /* Main-thread audio servicing at 1 ms granularity.  All pumping goes through
     * pumpAudioTick() so a CDDA/speech ISO read can't re-enter the mixer mid-mix
     * (see g_pumpBusy above).  Tried moving this to the 1 kHz timer ISR but
     * of_audio_write from IRQ produces audible vibrato; main-thread is the safer
     * spot even though it can underrun during long engine work. */
    OpenFPGAMixerManager *mgr = (OpenFPGAMixerManager *)_mixerManager;
    pumpAudioTick(mgr, true);
    while (msecs > 0) {
        usleep(1000);
        pumpAudioTick(mgr, (msecs & 0x3) == 0);   /* update (refill) every ~4 ms */
        /* Keep the cursor alive during the wait (~60 Hz internal gate); the flip
         * itself defers to the engine's own frames inside presentCursor(). */
        serviceCursorFromWork();
        --msecs;
    }
    pumpAudioTick(mgr, true);
}

/* Shared by pollEvent, delayMillis, and the graphics manager's
 * updateScreen (DMA blit can stall the main thread).  Pumping here
 * means audio stays gapless even when the engine doesn't return
 * control to its event loop for a frame.  initBackend stashes the
 * concrete manager pointers via openfpga_set_pump_managers so we
 * avoid the virtual-base cast from g_system. */
static DefaultTimerManager   *g_pumpTimerMgr = nullptr;
static OpenFPGAMixerManager  *g_pumpMixerMgr = nullptr;
/* g_pumpBusy is declared above delayMillis() (its per-ms pumps need it). */

void openfpga_set_pump_managers(Common::TimerManager *t, MixerManager *m) {
    g_pumpTimerMgr = (DefaultTimerManager *)t;
    g_pumpMixerMgr = (OpenFPGAMixerManager *)m;
}

void openfpga_drive_audio_and_timers(void) {
    if (!g_pumpTimerMgr || !g_pumpMixerMgr) return;
    if (g_pumpBusy) return;
    g_pumpBusy = true;
    g_pumpTimerMgr->checkTimers();
    openfpga_midi_pump_pending();
    extern void openfpga_audiocd_pump(void);
    openfpga_audiocd_pump();
    g_pumpMixerMgr->update();
    g_pumpMixerMgr->drain();
    g_pumpBusy = false;
}

void openfpga_mixer_pump_only(void) {
    if (!g_pumpMixerMgr || g_pumpBusy) return;
    g_pumpBusy = true;
    /* Light CDDA pump: this runs on latency-critical paths (just before a
     * frame blit in presentFrameInternal, between zip read chunks), where a
     * batched blocking refill would land in the frame time.  It still refills
     * below the emergency floor so audio can't underrun. */
    extern void openfpga_audiocd_pump_light(void);
    openfpga_audiocd_pump_light();
    g_pumpMixerMgr->update();
    g_pumpMixerMgr->drain();
    g_pumpBusy = false;
}

/* Drive audio + MIDI during long, non-yielding RENDER work -- chiefly SCI32
 * 640x480 frames where GfxFrameout composes many/large cels into the frame
 * buffer in one pass, with the data already resident (NO file read) and without
 * returning to delayMillis/pollEvent/updateScreen until the whole frame is
 * drawn.  SCI's own frame throttle is POST-frame (GfxFrameout::throttle runs
 * after frameOut completes), so a single heavy frameOut gets NO audio service
 * for its full duration and the ~120 ms ring cushion underruns -- the LSL7
 * talkie-animation dropout.  Called per screen item from drawScreenItemList so
 * the gap between pumps is one cel draw (a few ms) instead of a whole frame.
 *
 * Unlike openfpga_pump_during_load this deliberately does NOT arm the deep
 * cushion: frameOut runs every frame INCLUDING interactive play, where a deep
 * cushion would push newly-triggered click/SFX seconds back in the FIFO.  It
 * only tops the normal 120 ms ring and advances the MIDI melody so neither
 * stalls mid-frame.  Time-gated to ~6 ms (one mixer block is ~5.3 ms) so a
 * 90-item frame fires at most a handful of real pumps; reentrancy-safe via
 * openfpga_mixer_pump_only's g_pumpBusy guard and the MIDI parser's s_inProc
 * guard (a nested speech/CDDA read pulled by the mixer turns into a no-op). */
extern "C" void openfpga_pump_during_render(void) {
    extern void openfpga_midi_pump_pending(void);
    static uint32 s_lastRenderPumpMs = 0;
    uint32 now = of_time_ms();
    if ((uint32)(now - s_lastRenderPumpMs) < 6u)
        return;
    s_lastRenderPumpMs = now;
    openfpga_midi_pump_pending();
    openfpga_mixer_pump_only();
}

/* Drive audio + MIDI + timers during long, non-yielding engine work -- chiefly
 * SCI1/1.1 room/screen transitions that read+decompress large resource volumes
 * from the streamed ISO without ever returning to delayMillis / pollEvent /
 * updateScreen.  Without this the MIDI parser pump never fires (held notes
 * freeze) and the mixer's ~120 ms HW-ring cushion underruns, so the music stops
 * until the new screen is up.  Called from the FS read path -- the single choke
 * point all load traffic flows through.  Time-gated to ~8 ms (one mixer block is
 * ~5.3 ms) so a burst of block reads can't over-pump.  Reentrancy-safe: funnels
 * into openfpga_drive_audio_and_timers, whose g_pumpBusy guard turns a nested
 * speech/CDDA read (pulled by mgr->update()) into a no-op, and the MIDI parser's
 * s_inProc guard blocks parser re-entry. */
void openfpga_pump_during_load(void) {
    static uint32 s_lastPumpMs = 0;
    uint32 now = of_time_ms();

    /* Arm a DEEP audio cushion for the next ~600 ms, but only when reads are
     * BURSTING (a real load): the engine often follows a burst of reads with
     * >120 ms of pure in-memory work (resource decompress, room kAnimate
     * setup, the transition effect) during which NO read and NO updateScreen
     * fires -- so neither pump site runs and the normal 120 ms ring cushion
     * underruns (the "hiccup between loads").  Pre-filling a deep buffer here
     * rides through that gap.
     *
     * The burst gate matters: arming on EVERY read let a lone resource read
     * every couple of seconds in normal play flap the target 120ms->800ms->
     * drain->repeat; each arm made the mixer rebuild ~700 ms of buffer, and
     * with a rate-converted CDDA stream attached that rebuild is a heavy CPU
     * burst -- the MI1 CD-music picture stutter.  A genuine transition issues
     * dozens of reads within milliseconds, so requiring a few reads in quick
     * succession costs a real load nothing. */
    extern void openfpga_mixer_extend_cushion(uint32 untilMs);
    static uint32 s_burstStartMs = 0;
    static uint32 s_burstReads = 0;
    if ((uint32)(now - s_burstStartMs) > 150u) {
        s_burstStartMs = now;
        s_burstReads = 0;
    }
    ++s_burstReads;
    /* >=4 reads spanning >=50 ms: a real transition sustains both trivially
     * (reads every few ms for hundreds of ms); a lone resource access issues
     * a couple of 32 KB buffer fills inside a millisecond and passes neither.
     * The first 50 ms of a real load run unarmed, which the normal 120 ms
     * cushion covers. */
    if (s_burstReads >= 4u && (uint32)(now - s_burstStartMs) >= 50u)
        openfpga_mixer_extend_cushion(now + 600u);

    /* Keep the cursor moving through long non-yielding loads (SCI room/screen
     * transitions read+decompress large volumes without returning to delayMillis
     * / pollEvent).  serviceCursorFromWork() self-gates to ~60 Hz and, since the
     * engine isn't presenting during a load, presentCursor()'s engine-recent gate
     * passes so the cursor actually redraws.  Its re-entrancy guard swallows the
     * nested load read the mixer pump below may pull. */
    openfpga_service_cursor();

    if ((uint32)(now - s_lastPumpMs) < 8u)
        return;
    s_lastPumpMs = now;
    openfpga_drive_audio_and_timers();
}

void OSystem_OpenFPGA::getTimeAndDate(TimeDate &td, bool skipRecord) const {
    /* The Pocket has no RTC, but SCI's kGetTime(12h/24h) MUST advance: timed
     * Print/Dialog boxes (the "#time" auto-dismiss message/narration windows,
     * e.g. the LSL3 room-130 story recap) count their seconds down by watching
     * the packed h:m:s change once per wall-clock second.  A fixed time froze
     * that countdown, so every such box across ALL SCI games failed to resolve
     * (never dismissed / never shown).  Synthesize an advancing clock from
     * uptime so the seconds tick. */
    uint32 secs = (of_time_ms() - _startTime) / 1000;
    td.tm_sec  = (int)(secs % 60);
    td.tm_min  = (int)((secs / 60) % 60);
    td.tm_hour = (int)((secs / 3600) % 24);
    td.tm_mday = 1;
    td.tm_mon  = 0;
    td.tm_year = 125; /* 2025 - 1900 */
    td.tm_wday = 3;
}

void OSystem_OpenFPGA::quit() {
    for (;;) {}
}

void OSystem_OpenFPGA::logMessage(LogMessageType::Type type, const char *message) {
    /* Show latest message at the bottom two lines (28-29).  Use the
     * VT100 "Erase in Line" escape (\x1b[2K) to clear each row -- vs
     * writing 40 literal spaces, which used to flood phdpd's mirrored
     * byte stream with whitespace even though the on-screen result was
     * the same. */
    char buf[80];
    strncpy(buf, message, 79);
    buf[79] = '\0';
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    printf("\x1b[28;1H\x1b[2K\x1b[29;1H\x1b[2K\x1b[28;1H%s\r\n", buf);
    fflush(stdout);
}

void OSystem_OpenFPGA::addSysArchivesToSearchSet(Common::SearchSet &s, int priority) {
    /* No system archives on this platform */
}

/* ScummVM's persistent config slot is per-game (monkey1.ini etc.).
 * main() resolves the actual filename via opendir at startup and
 * stashes it here. */

namespace {

char g_iniPath[256] = "";
const char *iniPath() { return g_iniPath[0] ? g_iniPath : nullptr; }

class ConfigWriteStream : public Common::MemoryWriteStreamDynamic {
public:
    ConfigWriteStream(const char *path)
        : Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES),
          _path(path), _flushed(false), _err(false) {}

    ~ConfigWriteStream() override { flushToSlot(); }

    bool flush() override { flushToSlot(); return !_err; }
    bool err() const override { return _err || Common::MemoryWriteStreamDynamic::err(); }

private:
    void flushToSlot() {
        if (_flushed) return;
        _flushed = true;
        FILE *f = fopen(_path, "wb");
        if (!f) { _err = true; return; }
        uint32 n = size();
        if (n && fwrite(getData(), 1, n, f) != n) _err = true;
        fclose(f);
    }
    const char *_path;
    bool        _flushed;
    bool        _err;
};

} // namespace

void openfpga_set_config_path(const char *path) {
    if (!path) { g_iniPath[0] = '\0'; return; }
    snprintf(g_iniPath, sizeof(g_iniPath), "%s", path);
    of_file_slot_register(9, g_iniPath);
}

Common::SeekableReadStream *OSystem_OpenFPGA::createConfigReadStream() {
    const char *path = iniPath();
    if (!path) return nullptr;
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return nullptr; }
    fseek(f, 0, SEEK_SET);
    byte *buf = (byte *)malloc((size_t)sz);
    if (!buf) { fclose(f); return nullptr; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return nullptr; }
    fclose(f);
    return new Common::MemoryReadStream(buf, (uint32)sz, DisposeAfterUse::YES);
}

Common::WriteStream *OSystem_OpenFPGA::createConfigWriteStream() {
    /* Writes to the nonvolatile config slot currently starve the
     * launcher's UART pump on the Pocket -- same shape as the ISO read
     * issue the cdiso CR flagged for the OS-side yielding cache-fill
     * follow-up.  Until that lands, refuse config persistence so
     * ScummVM's setAndFlush calls (notably updateGameGUIOptions during
     * createInstance) early-return from flushToDisk instead of
     * blocking.  Engine options stay in-memory for the session. */
    return nullptr;
    (void)iniPath();
}

Common::Path OSystem_OpenFPGA::getDefaultConfigFileName() {
    const char *path = iniPath();
    return path ? Common::Path(path) : Common::Path();
}

Common::Path OSystem_OpenFPGA::getDefaultLogFileName() {
    return Common::Path();
}

void OSystem_OpenFPGA::messageBox(LogMessageType::Type type, const char *message) {
    /* Don't show GUI dialog — just log and continue.  Use \x1b[2K to
     * clear each row instead of padding spaces. */
    char buf[41];
    strncpy(buf, message, 40);
    buf[40] = '\0';
    printf("\x1b[25;1H\x1b[2KMSGBOX:\x1b[26;1H\x1b[2K%s", buf);
    fflush(stdout);
}

void OSystem_OpenFPGA::fatalError() {
    /* Show the terminal so the user can read the panic message even
     * if the engine had switched to framebuffer-only display. */
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\x1b[27;1H\x1b[2K*** FATAL ERROR — halted ***");
    fflush(stdout);
    for (;;) { usleep(100 * 1000); }
}
