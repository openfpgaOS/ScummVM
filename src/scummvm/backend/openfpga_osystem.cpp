/*
 * openfpga_osystem.cpp -- ScummVM OSystem backend for openfpgaOS
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_osystem.h"

#include "backends/mutex/null/null-mutex.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "common/memstream.h"
#include "common/stream.h"
#include "openfpga_audiocd.h"
#include "openfpga_fs.h"
#include "openfpga_midi.h"
#include "openfpga_mixer.h"
#include "openfpga_save.h"
#include "splash_logo.h"
#include "audio/mixer.h"

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

OpenFPGAGraphicsManager::OpenFPGAGraphicsManager()
    : _screenW(320), _screenH(200), _screenChangeID(0),
      _overlayVisible(false), _paletteDirty(false), _screenDirty(false),
      _cursorX(160), _cursorY(100), _cursorHotX(0), _cursorHotY(0),
      _cursorW(0), _cursorH(0), _cursorKeycolor(0), _cursorVisible(false),
      _gpuReady(false), _videoBufIdx(-1), _videoFence(0), _gpuCleanMask(0) {
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
        fbW = OPENFPGA_SCREEN_W;
        fbH = OPENFPGA_SCREEN_H;
        fbStride = OPENFPGA_SCREEN_W;
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

static void configureFramebufferMode() {
    of_video_mode_t mode;
    memset(&mode, 0, sizeof(mode));
    mode.width = OPENFPGA_SCREEN_W;
    mode.height = OPENFPGA_SCREEN_H;
    mode.color_mode = OF_VIDEO_MODE_8BIT;

    if (of_video_set_mode(&mode) != 0)
        of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
}

void OpenFPGAGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
    if (width > OPENFPGA_SCREEN_W)
        width = OPENFPGA_SCREEN_W;
    if (height > OPENFPGA_SCREEN_H)
        height = OPENFPGA_SCREEN_H;
    _screenW = width;
    _screenH = height;
    memset(_screenBuf, 0, sizeof(_screenBuf));
    _screenChangeID++;
    _screenDirty = true;

    /* Game graphics only -- main() will flip to FRAMEBUFFER right
     * before engine->run().  Re-assert the 8-bit mode here, but do NOT
     * clear/flip: the engine calls initSize() at the very start of
     * run(), long before its slow data load finishes and it pushes a
     * first frame.  Clearing here (and via the legacy of_video_flip path,
     * which also desyncs the GPU triple-buffer rotation set up by the
     * splash) would blank the ScummVM logo for the whole load.  Leave the
     * splash on the framebuffer; the first updateScreen() replaces it
     * (clearFrameBorders + full-screen copy handle the size change). */
    configureFramebufferMode();
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
    for (int row = 0; row < h; row++) {
        memcpy(dst, src, w);
        dst += _screenW;
        src += pitch;
    }
    _screenDirty = true;
}

Graphics::Surface *OpenFPGAGraphicsManager::lockScreen() {
    _frameSurface.init(_screenW, _screenH, _screenW,
                       _screenBuf, Graphics::PixelFormat::createFormatCLUT8());
    return &_frameSurface;
}

void OpenFPGAGraphicsManager::unlockScreen() {
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::fillScreen(uint32 col) {
    memset(_screenBuf, col & 0xFF, _screenW * _screenH);
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

void OpenFPGAGraphicsManager::clearFrameBorders(uint8_t *fb, uint fbW,
                                                uint fbH, uint fbStride,
                                                int xOff, int yOff,
                                                uint copyW, uint copyH) {
    if (!_gpuReady || !fb) {
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

    if (issued)
        of_gpu_wait(of_gpu_submit());
}

void OpenFPGAGraphicsManager::presentFrame() {
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
    /* Pump audio (mixer only -- no timer/MIDI recursion) before and
     * after presentation; the frame copy/flip can stall long enough to
     * drain the 21 ms audio FIFO. */
    extern void openfpga_mixer_pump_only(void);
    openfpga_mixer_pump_only();

    if (_paletteDirty) {
        uint32_t pal32[256];
        for (int i = 0; i < 256; i++) {
            pal32[i] = ((uint32_t)_palette[i*3]     << 16) |
                       ((uint32_t)_palette[i*3 + 1] <<  8) |
                        (uint32_t)_palette[i*3 + 2];
        }
        of_video_palette_bulk(pal32, 256);
        _paletteDirty = false;
    }

    /* Always blit -- engines expect updateScreen() to push every frame
     * regardless of dirty tracking. */
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

    clearFrameBorders(fb, fbW, fbH, fbStride, xOff, yOff, copyW, copyH);
    for (uint y = 0; y < copyH; ++y) {
        memcpy(fb + (y + yOff) * fbStride + xOff,
               _screenBuf + y * _screenW,
               copyW);
    }

    if (_cursorVisible)
        drawCursor(fb, fbW, fbH, fbStride, xOff, yOff);

    of_cache_flush_range(fb, fbStride * fbH);
    presentFrame();
    _screenDirty = false;

    extern void openfpga_mixer_pump_only(void);
    openfpga_mixer_pump_only();
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
    memcpy(_screenBuf, SPLASH_PIX, SPLASH_W * SPLASH_H);
    for (int i = 0; i < 256; i++) {
        _palette[i * 3]     = (uint8_t)(SPLASH_PAL[i] >> 16);
        _palette[i * 3 + 1] = (uint8_t)(SPLASH_PAL[i] >> 8);
        _palette[i * 3 + 2] = (uint8_t)(SPLASH_PAL[i]);
    }
    for (int n = 0; n < 3; n++) {
        _paletteDirty = true;
        updateScreen();
    }
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
                                         uint fbStride, int xOff,
                                         int yOff) const {
    if (!_cursorVisible || _cursorW == 0 || _cursorH == 0)
        return;

    int cx = _cursorX - _cursorHotX + xOff;
    int cy = _cursorY - _cursorHotY + yOff;

    for (uint row = 0; row < _cursorH; row++) {
        int sy = cy + (int)row;
        if (sy < 0 || sy >= (int)fbH) continue;
        for (uint col = 0; col < _cursorW; col++) {
            int sx = cx + (int)col;
            if (sx < 0 || sx >= (int)fbW) continue;
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
      _ignoreInitialButtons(true), _keypadMode(false),
      _copyProtectActive(false), _copyProtectKeys(0),
      _masterVolume(255), _musicVolume(192),
      _selectHeld(false), _selectConsumed(false),
      _lastMouseTick(0),
      _mouseAccumX(0), _mouseAccumY(0) {
}

OSystem_OpenFPGA::~OSystem_OpenFPGA() {
}

void OSystem_OpenFPGA::initBackend() {
    /* Guard against double init (main calls us, then scummvm_main calls us again) */
    if (_ofGfx)
        return;

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

bool OSystem_OpenFPGA::pollEvent(Common::Event &event) {
    openfpga_drive_audio_and_timers();

    if (popQueuedEvent(event))
        return true;

    of_input_poll();

    /* Auto-dismiss GUI dialogs: send Return keypress once after startup.
     * This handles the "unknown game version" dialog that blocks. */
    if (_autoDismissCounter > 0) {
        _autoDismissCounter--;
        if (_autoDismissCounter == 0) {
            queueKey(Common::KEYCODE_RETURN, Common::ASCII_RETURN);
            return popQueuedEvent(event);
        }
    }

    /* Analog stick and D-pad both drive the mouse.  Movement is applied
     * before button handling so clicks land at the current cursor, but
     * button/key events are returned first so continuous movement cannot
     * starve clicks or shortcuts. */
    of_input_state_t state;
    of_input_state(0, &state);

    if (_ignoreInitialButtons) {
        if (state.buttons != 0) {
            _lastMouseTick = getMillis();
            _mouseAccumX = 0;
            _mouseAccumY = 0;
            _mouseButtonL = false;
            _mouseButtonR = false;
            return false;
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
        if (state.buttons_pressed & OF_BTN_UP) {
            _masterVolume = (_masterVolume + 16 > 255) ? 255 : _masterVolume + 16;
            of_mixer_set_master_volume(_masterVolume);
            _selectConsumed = true;
        } else if (state.buttons_pressed & OF_BTN_DOWN) {
            _masterVolume = (_masterVolume - 16 < 0) ? 0 : _masterVolume - 16;
            of_mixer_set_master_volume(_masterVolume);
            _selectConsumed = true;
        } else if (state.buttons_pressed & (OF_BTN_LEFT | OF_BTN_RIGHT)) {
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
        }
        _selectHeld = true;
        return false;
    }
    if (_selectHeld) {                  /* SELECT just released */
        _selectHeld = false;
        if (!_selectConsumed)
            _keypadMode = !_keypadMode; /* tap = toggle keypad */
        _selectConsumed = false;
        return false;
    }
    if (_keypadMode) {
        static const struct { uint32 btn; Common::KeyCode kc; uint16 ch; } kp[] = {
            { OF_BTN_UP,    Common::KEYCODE_1, '1' }, { OF_BTN_DOWN,  Common::KEYCODE_2, '2' },
            { OF_BTN_LEFT,  Common::KEYCODE_3, '3' }, { OF_BTN_RIGHT, Common::KEYCODE_4, '4' },
            { OF_BTN_A,     Common::KEYCODE_5, '5' }, { OF_BTN_B,     Common::KEYCODE_6, '6' },
            { OF_BTN_X,     Common::KEYCODE_7, '7' }, { OF_BTN_Y,     Common::KEYCODE_8, '8' },
            { OF_BTN_L1,    Common::KEYCODE_9, '9' }, { OF_BTN_R1,    Common::KEYCODE_0, '0' },
        };
        for (uint i = 0; i < sizeof(kp) / sizeof(kp[0]); ++i) {
            if (state.buttons_pressed & kp[i].btn) {
                queueKey(kp[i].kc, kp[i].ch);
                return popQueuedEvent(event);
            }
        }
        if (state.buttons_pressed & OF_BTN_START) {
            _keypadMode = false;   /* submit + auto-exit so the mouse returns */
            queueKey(Common::KEYCODE_RETURN, Common::ASCII_RETURN);
            return popQueuedEvent(event);
        }
        return false;   /* swallow movement/clicks while in keypad mode */
    }

    uint32 nowMs = getMillis();
    uint32 elapsedMs = (_lastMouseTick == 0) ? 16 : nowMs - _lastMouseTick;
    _lastMouseTick = nowMs;
    if (elapsedMs > 50)
        elapsedMs = 50;

    const int deadzone = 4000;
    const bool slowMouse = (state.buttons & (OF_BTN_L1 | OF_BTN_L2)) != 0;
    const bool fastMouse = (state.buttons & (OF_BTN_R1 | OF_BTN_R2)) != 0;
    const int analogMaxRate = slowMouse ? 120 : (fastMouse ? 420 : 260);
    const int dpadBaseRate = slowMouse ? 60 : (fastMouse ? 240 : 120);
    int lx = state.joy_lx;
    int ly = state.joy_ly;
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

    /* A = left click */
    bool aDown = (state.buttons & OF_BTN_A) != 0;
    if (aDown != _mouseButtonL) {
        _mouseButtonL = aDown;
        event.type = aDown ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
        event.mouse.x = _ofGfx->getMouseX();
        event.mouse.y = _ofGfx->getMouseY();
        return true;
    }

    /* B = right click */
    bool bDown = (state.buttons & OF_BTN_B) != 0;
    if (bDown != _mouseButtonR) {
        _mouseButtonR = bDown;
        event.type = bDown ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;
        event.mouse.x = _ofGfx->getMouseX();
        event.mouse.y = _ofGfx->getMouseY();
        return true;
    }

    /* Controller shortcuts. */
    if (state.buttons_pressed & OF_BTN_X) {
        queueKey(Common::KEYCODE_RETURN, Common::ASCII_RETURN);
        return popQueuedEvent(event);
    }

    if (state.buttons_pressed & OF_BTN_Y) {
        queueKey(Common::KEYCODE_SPACE, Common::ASCII_SPACE);
        return popQueuedEvent(event);
    }

    if (state.buttons_pressed & OF_BTN_START) {
        queueKey(Common::KEYCODE_F5, Common::ASCII_F5);
        return popQueuedEvent(event);
    }

    /* Plain Select intentionally has no Escape binding. SCUMM uses Escape
     * as cutscene-abort, so mapping Select there skips to the next scene. */

    if (state.buttons_pressed & OF_BTN_L3) {
        queueKey(Common::KEYCODE_TAB, Common::ASCII_TAB);
        return popQueuedEvent(event);
    }

    if (state.buttons_pressed & OF_BTN_R3) {
        queueKey(Common::KEYCODE_PERIOD, '.');
        return popQueuedEvent(event);
    }

    if (moved) {
        event.type = Common::EVENT_MOUSEMOVE;
        event.mouse.x = _ofGfx->getMouseX();
        event.mouse.y = _ofGfx->getMouseY();
        return true;
    }

    return false;
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
    return new NullMutexInternal();
}

uint32 OSystem_OpenFPGA::getMillis(bool skipRecord) {
    return of_time_ms() - _startTime;
}

void OSystem_OpenFPGA::delayMillis(uint msecs) {
    /* Main-thread audio servicing at 1 ms granularity.  Each tick:
     *   - drain() advances the SDK software-mixer voices (cheap)
     *   - update() mixes + pushes a block to the HW ring when it has room
     * Tried moving this to the 1 kHz timer ISR but of_audio_write from IRQ
     * produces audible vibrato; main-thread is the safer spot even though it
     * can underrun during long engine work. */
    OpenFPGAMixerManager *mgr = (OpenFPGAMixerManager *)_mixerManager;
    extern void openfpga_audiocd_pump(void);
    openfpga_midi_pump_pending();
    openfpga_audiocd_pump();
    mgr->update();
    mgr->drain();
    while (msecs > 0) {
        usleep(1000);
        openfpga_midi_pump_pending();
        openfpga_audiocd_pump();
        mgr->drain();
        if ((msecs & 0x3) == 0) mgr->update();   /* refill every ~4 ms */
        --msecs;
    }
    openfpga_midi_pump_pending();
    mgr->update();
    mgr->drain();
}

/* Shared by pollEvent, delayMillis, and the graphics manager's
 * updateScreen (DMA blit can stall the main thread).  Pumping here
 * means audio stays gapless even when the engine doesn't return
 * control to its event loop for a frame.  initBackend stashes the
 * concrete manager pointers via openfpga_set_pump_managers so we
 * avoid the virtual-base cast from g_system. */
static DefaultTimerManager   *g_pumpTimerMgr = nullptr;
static OpenFPGAMixerManager  *g_pumpMixerMgr = nullptr;
static bool                   g_pumpBusy = false;

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
    extern void openfpga_audiocd_pump(void);
    openfpga_audiocd_pump();
    g_pumpMixerMgr->update();
    g_pumpMixerMgr->drain();
    g_pumpBusy = false;
}

void OSystem_OpenFPGA::getTimeAndDate(TimeDate &td, bool skipRecord) const {
    /* No RTC on Pocket — return a fixed date */
    td.tm_sec = 0;
    td.tm_min = 0;
    td.tm_hour = 12;
    td.tm_mday = 1;
    td.tm_mon = 0;
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
