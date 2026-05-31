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

OpenFPGAGraphicsManager::OpenFPGAGraphicsManager()
    : _screenW(320), _screenH(200), _screenChangeID(0),
      _overlayVisible(false), _paletteDirty(false), _screenDirty(false),
      _cursorX(160), _cursorY(100), _cursorHotX(0), _cursorHotY(0),
      _cursorW(0), _cursorH(0), _cursorKeycolor(0), _cursorVisible(false),
      _gpuReady(false), _videoBufIdx(-1), _videoFence(0), _gpuCleanMask(0) {
    memset(_screenBuf, 0, sizeof(_screenBuf));
    memset(_palette, 0, sizeof(_palette));
    memset(_cursorData, 0, sizeof(_cursorData));
}

OpenFPGAGraphicsManager::~OpenFPGAGraphicsManager() {
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
     * before engine->run().  Configure the color mode and clear here. */
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
    of_video_clear(0);
    of_video_flip();
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
        of_cache_flush_range(fb, OPENFPGA_SCREEN_W * OPENFPGA_SCREEN_H);
        _gpuCleanMask |= (1u << _videoBufIdx);
    }
    return fb;
}

void OpenFPGAGraphicsManager::clearFrameBorders(uint8_t *fb, int xOff, int yOff,
                                                uint copyW, uint copyH) {
    if (!_gpuReady || !fb) {
        if (fb)
            memset(fb, 0, OPENFPGA_SCREEN_W * OPENFPGA_SCREEN_H);
        return;
    }

    const uint32_t fbAddr = (uint32_t)(uintptr_t)fb;
    bool issued = false;
    of_gpu_set_framebuffer(fbAddr, OPENFPGA_SCREEN_W);

    if (yOff > 0) {
        of_gpu_clear_rect(fbAddr, OPENFPGA_SCREEN_W, yOff, 0);
        issued = true;
    }

    const int bottomY = yOff + (int)copyH;
    if (bottomY < OPENFPGA_SCREEN_H) {
        of_gpu_clear_rect(fbAddr + (uint32_t)bottomY * OPENFPGA_SCREEN_W,
                          OPENFPGA_SCREEN_W, OPENFPGA_SCREEN_H - bottomY, 0);
        issued = true;
    }

    if (copyH != 0 && xOff > 0) {
        of_gpu_clear_rect(fbAddr + (uint32_t)yOff * OPENFPGA_SCREEN_W,
                          xOff, copyH, 0);
        issued = true;
    }

    const int rightX = xOff + (int)copyW;
    if (copyH != 0 && rightX < OPENFPGA_SCREEN_W) {
        of_gpu_clear_rect(fbAddr + (uint32_t)yOff * OPENFPGA_SCREEN_W + rightX,
                          OPENFPGA_SCREEN_W - rightX, copyH, 0);
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
    int xOff = ((int)OPENFPGA_SCREEN_W - (int)_screenW) / 2;
    int yOff = ((int)OPENFPGA_SCREEN_H - (int)_screenH) / 2;
    if (xOff < 0) xOff = 0;
    if (yOff < 0) yOff = 0;
    uint copyW = _screenW;
    uint copyH = _screenH;
    if (copyW > OPENFPGA_SCREEN_W) copyW = OPENFPGA_SCREEN_W;
    if (copyH > OPENFPGA_SCREEN_H) copyH = OPENFPGA_SCREEN_H;

    clearFrameBorders(fb, xOff, yOff, copyW, copyH);
    for (uint y = 0; y < copyH; ++y) {
        memcpy(fb + (y + yOff) * OPENFPGA_SCREEN_W + xOff,
               _screenBuf + y * _screenW,
               copyW);
    }

    if (_cursorVisible)
        drawCursor(fb, xOff, yOff);

    of_cache_flush_range(fb, OPENFPGA_SCREEN_W * OPENFPGA_SCREEN_H);
    presentFrame();
    _screenDirty = false;

    extern void openfpga_mixer_pump_only(void);
    openfpga_mixer_pump_only();
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

void OpenFPGAGraphicsManager::drawCursor(uint8_t *dst, int xOff, int yOff) const {
    if (!_cursorVisible || _cursorW == 0 || _cursorH == 0)
        return;

    int cx = _cursorX - _cursorHotX + xOff;
    int cy = _cursorY - _cursorHotY + yOff;

    for (uint row = 0; row < _cursorH; row++) {
        int sy = cy + (int)row;
        if (sy < 0 || sy >= OPENFPGA_SCREEN_H) continue;
        for (uint col = 0; col < _cursorW; col++) {
            int sx = cx + (int)col;
            if (sx < 0 || sx >= OPENFPGA_SCREEN_W) continue;
            uint8_t px = _cursorData[row * 64 + col];
            if (px != (_cursorKeycolor & 0xFF))
                dst[sy * OPENFPGA_SCREEN_W + sx] = px;
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
      _ignoreInitialButtons(true), _lastMouseTick(0),
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

Common::MutexInternal *OSystem_OpenFPGA::createMutex() {
    return new NullMutexInternal();
}

uint32 OSystem_OpenFPGA::getMillis(bool skipRecord) {
    return of_time_ms() - _startTime;
}

void OSystem_OpenFPGA::delayMillis(uint msecs) {
    /* Main-thread drain at 1 ms granularity.  Each tick:
     *   - drain() pushes ring -> FIFO (cheap)
     *   - update() refills the ring if below 75 % (no-op when full)
     * Tried moving drain to the 1 kHz timer ISR but of_audio_write
     * from IRQ produces audible vibrato; main-thread is the safer
     * spot even though it can underrun during long engine work. */
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
